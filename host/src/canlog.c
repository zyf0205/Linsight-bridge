#include "canlog.h"

#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char LSC_MAGIC[4] = {'L', 'S', 'C', '1'};

typedef struct {
    uint32_t bitrate;
    uint32_t flags;
    uint64_t start_wall;
    uint32_t boot_count;
    uint32_t anchor_ms;
} lsc_header_t;

static int lsc_read_header(FILE *f, lsc_header_t *h)
{
    uint8_t hdr[LSC_HEADER_SIZE];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, LSC_MAGIC, 4) != 0 ||
        linsight_get_u16(hdr + 4) != LSC_VERSION) {
        errno = EPROTO;
        return -1;
    }
    memset(h, 0, sizeof(*h));
    h->bitrate = linsight_get_u32(hdr + 6);
    h->flags = linsight_get_u32(hdr + 10);
    for (int i = 0; i < 8; i++) {
        h->start_wall |= (uint64_t)hdr[16 + i] << (8 * i);
    }
    h->boot_count = linsight_get_u32(hdr + 24);
    h->anchor_ms = linsight_get_u32(hdr + 28);
    if (h->anchor_ms > 999u) {
        h->anchor_ms = 0; /* untrusted or older file */
    }
    return 0;
}

int lsc_writer_open_ex(lsc_writer_t *w, const char *path, uint32_t bitrate,
                       const lsc_open_info_t *info)
{
    memset(w, 0, sizeof(*w));
    w->f = fopen(path, "wb");
    if (!w->f) {
        return -1;
    }

    const uint64_t start_wall_ms = info ? info->start_wall_ms : 0;
    const uint32_t flags = info ? info->flags : 0;
    const uint32_t boot_count = info ? info->boot_count : 0;
    const uint64_t start_wall = start_wall_ms / 1000u;
    const uint32_t anchor_ms = (uint32_t)(start_wall_ms % 1000u);

    uint8_t hdr[LSC_HEADER_SIZE] = {0};
    memcpy(hdr, LSC_MAGIC, 4);
    hdr[4] = LSC_VERSION & 0xFFu;
    hdr[5] = (LSC_VERSION >> 8) & 0xFFu;
    linsight_put_u32(hdr + 6, bitrate);
    linsight_put_u32(hdr + 10, flags);
    for (int i = 0; i < 8; i++) {
        hdr[16 + i] = (uint8_t)(start_wall >> (8 * i));
    }
    linsight_put_u32(hdr + 24, boot_count);
    linsight_put_u32(hdr + 28, anchor_ms);

    w->bitrate = bitrate;
    w->flags = flags;
    w->boot_count = boot_count;
    w->start_wall = start_wall;
    w->start_wall_ms = start_wall_ms;
    if (fwrite(hdr, 1, LSC_HEADER_SIZE, w->f) != LSC_HEADER_SIZE) {
        fclose(w->f);
        w->f = NULL;
        return -1;
    }
    return 0;
}

int lsc_writer_open(lsc_writer_t *w, const char *path, uint32_t bitrate, uint64_t start_wall)
{
    const lsc_open_info_t info = {
        .start_wall_ms = start_wall * 1000u,
        .flags = 0,
        .boot_count = 0,
    };
    return lsc_writer_open_ex(w, path, bitrate, &info);
}

int lsc_writer_write(lsc_writer_t *w, const linsight_can_record_t *r)
{
    if (!w->f) {
        return -1; /* caller must not write to a closed log */
    }
    uint8_t buf[LINSIGHT_REC_SIZE];
    linsight_record_pack(r, buf);
    if (fwrite(buf, 1, LINSIGHT_REC_SIZE, w->f) != LINSIGHT_REC_SIZE) {
        return -1;
    }
    w->written++;
    return 0;
}

int lsc_writer_flush(lsc_writer_t *w)
{
    return fflush(w->f) == 0 ? 0 : -1;
}

int lsc_writer_sync(lsc_writer_t *w)
{
    if (!w->f || fflush(w->f) != 0) {
        return -1;
    }
    return fdatasync(fileno(w->f)) == 0 ? 0 : -1;
}

void lsc_writer_close(lsc_writer_t *w)
{
    if (w->f) {
        fflush(w->f);
        fclose(w->f);
        w->f = NULL;
    }
}

int lsc_reader_open(lsc_reader_t *r, const char *path)
{
    memset(r, 0, sizeof(*r));
    r->f = fopen(path, "rb");
    if (!r->f) {
        return -1;
    }
    lsc_header_t h;
    if (lsc_read_header(r->f, &h) != 0) {
        fclose(r->f);
        r->f = NULL;
        return -1;
    }
    r->bitrate = h.bitrate;
    r->flags = h.flags;
    r->boot_count = h.boot_count;
    r->anchor_ms = h.anchor_ms;
    r->start_wall = h.start_wall;
    r->start_wall_ms = h.start_wall * 1000u + h.anchor_ms;
    r->wall_valid = (h.flags & LSC_FLAG_TS_UPTIME_ONLY) == 0;
    /* Snapshot the record count: replaying the daemon's current .lsc while the
     * daemon keeps appending would otherwise never reach EOF (and would
     * re-send the replayed frames forever). */
    if (fseeko(r->f, 0, SEEK_END) != 0) {
        fclose(r->f);
        r->f = NULL;
        return -1;
    }
    const off_t end = ftello(r->f);
    if (end < (off_t)LSC_HEADER_SIZE || fseeko(r->f, LSC_HEADER_SIZE, SEEK_SET) != 0) {
        fclose(r->f);
        r->f = NULL;
        errno = EPROTO;
        return -1;
    }
    r->remaining = (uint64_t)(end - (off_t)LSC_HEADER_SIZE) / LINSIGHT_REC_SIZE;
    return 0;
}

int lsc_reader_next(lsc_reader_t *r, linsight_can_record_t *rec)
{
    if (r->remaining == 0) {
        return 0;
    }
    uint8_t buf[LINSIGHT_REC_SIZE];
    const size_t n = fread(buf, 1, sizeof(buf), r->f);
    if (n == 0) {
        return ferror(r->f) ? -1 : 0;
    }
    if (n != sizeof(buf)) {
        /* Tolerate a truncated tail record (crash during the last write). */
        return feof(r->f) ? 0 : -1;
    }
    r->remaining--;
    linsight_record_unpack(buf, rec);
    /* The file may be untrusted (truncated tail, foreign writer): clamp
     * dlc so export/replay loops never read past data[]. */
    if (rec->dlc > LINSIGHT_DLC_MAX) {
        rec->dlc = LINSIGHT_DLC_MAX;
    }
    return 1;
}

void lsc_reader_close(lsc_reader_t *r)
{
    if (r->f) {
        fclose(r->f);
        r->f = NULL;
    }
}

long lsc_read(const char *path, lsc_record_cb cb, void *ctx)
{
    lsc_reader_t r;
    if (lsc_reader_open(&r, path) != 0) {
        return -1;
    }

    long count = 0;
    linsight_can_record_t rec;
    int rc;
    while ((rc = lsc_reader_next(&r, &rec)) == 1) {
        cb(&rec, ctx);
        count++;
    }
    lsc_reader_close(&r);
    return rc < 0 ? -1 : count;
}

/* ------------------------------------------------------------ exports */

typedef struct {
    FILE *out;
    uint64_t start_wall_ms;
    uint32_t bitrate;
    const lsc_filter_t *filter;
    long count;
    bool asc; /* write the Vector ASC header/footer */
} export_ctx_t;

static bool export_match(const linsight_can_record_t *r, const export_ctx_t *e)
{
    const lsc_filter_t *f = e->filter;
    if (!f) {
        return true;
    }
    if (f->id_valid && r->id != f->id) {
        return false;
    }
    if (f->ext >= 0 && ((r->flags & LINSIGHT_REC_FLAG_EXT) ? 1 : 0) != f->ext) {
        return false;
    }
    const uint64_t wall = (e->start_wall_ms + (uint64_t)r->ts_ms) / 1000u;
    if (f->from_valid && wall < f->from_wall) {
        return false;
    }
    if (f->to_valid && wall >= f->to_wall) {
        return false;
    }
    return true;
}

/* Vector ASC: "<relative seconds> 1  <ID>[x]  Rx  d|r <dlc> <data...>". */
static void export_asc_cb(const linsight_can_record_t *r, void *ctx)
{
    export_ctx_t *e = ctx;
    if (!export_match(r, e)) {
        return;
    }
    const double ts = (double)r->ts_ms / 1000.0;
    const bool ext = (r->flags & LINSIGHT_REC_FLAG_EXT) != 0;
    const char type = (r->flags & LINSIGHT_REC_FLAG_RTR) ? 'r' : 'd';

    if (ext) {
        fprintf(e->out, "%10.6f 1  %08Xx", ts, (unsigned)r->id);
    } else {
        fprintf(e->out, "%10.6f 1  %03X", ts, (unsigned)r->id);
    }
    fprintf(e->out, "            Rx   %c %u", type, (unsigned)r->dlc);
    if (!(r->flags & LINSIGHT_REC_FLAG_RTR)) {
        for (int i = 0; i < r->dlc; i++) {
            fprintf(e->out, " %02X", r->data[i]);
        }
    }
    fputc('\n', e->out);
    e->count++;
}

/* can-utils candump log format: "(epoch.microseconds) can0 123#1122". */
static void export_candump_cb(const linsight_can_record_t *r, void *ctx)
{
    export_ctx_t *e = ctx;
    if (!export_match(r, e)) {
        return;
    }
    char line[128];
    const int n = canlog_format_candump_ms(r, e->start_wall_ms, line, sizeof(line));
    if (n > 0) {
        fwrite(line, 1, (size_t)n, e->out);
        fputc('\n', e->out);
        e->count++;
    }
}

static long export_common(const char *lsc_path, const char *out_path, lsc_record_cb cb,
                          const lsc_filter_t *filter)
{
    if (out_path && strcmp(out_path, "-") != 0 && strcmp(out_path, lsc_path) == 0) {
        errno = EINVAL;
        return -1;
    }

    lsc_reader_t rd;
    if (lsc_reader_open(&rd, lsc_path) != 0) {
        return -1;
    }

    export_ctx_t e = {
        .start_wall_ms = rd.start_wall_ms,
        .bitrate = rd.bitrate,
        .filter = filter,
        .asc = cb == export_asc_cb,
    };
    e.out = (strcmp(out_path, "-") == 0) ? stdout : fopen(out_path, "w");
    if (!e.out) {
        lsc_reader_close(&rd);
        return -1;
    }

    if (e.asc) {
        char date[64] = "Thu Jan  1 00:00:00 1970";
        const time_t wall =
            rd.start_wall_ms ? (time_t)(rd.start_wall_ms / 1000u) : time(NULL);
        struct tm tm;
        if (localtime_r(&wall, &tm)) {
            (void)strftime(date, sizeof(date), "%a %b %e %H:%M:%S %Y", &tm);
        }
        fprintf(e.out, "date %s\n", date);
        fprintf(e.out, "base hex  timestamps absolute\n");
        fprintf(e.out, "internal events logged\n");
        fprintf(e.out, "// version 9.0.0\n");
        fprintf(e.out, "Begin Triggerblock %s\n", date);
        fprintf(e.out, "   0.000000 Start of measurement\n");
    }

    linsight_can_record_t rec;
    int rc;
    while ((rc = lsc_reader_next(&rd, &rec)) == 1) {
        cb(&rec, &e);
    }
    lsc_reader_close(&rd);
    if (e.asc) {
        fprintf(e.out, "End Triggerblock\n");
    }
    if (e.out != stdout && fclose(e.out) != 0) {
        return -1;
    }
    return rc < 0 ? -1 : e.count;
}

long canlog_export_asc(const char *lsc_path, const char *out_path)
{
    return export_common(lsc_path, out_path, export_asc_cb, NULL);
}

long canlog_export_candump(const char *lsc_path, const char *out_path)
{
    return export_common(lsc_path, out_path, export_candump_cb, NULL);
}

long canlog_export_asc_filtered(const char *lsc_path, const char *out_path,
                                const lsc_filter_t *filter)
{
    return export_common(lsc_path, out_path, export_asc_cb, filter);
}

long canlog_export_candump_filtered(const char *lsc_path, const char *out_path,
                                    const lsc_filter_t *filter)
{
    return export_common(lsc_path, out_path, export_candump_cb, filter);
}

int canlog_format_candump_ms(const linsight_can_record_t *r, uint64_t start_wall_ms, char *buf,
                             size_t cap)
{
    const uint64_t wall_ms = start_wall_ms + (uint64_t)r->ts_ms;
    const uint64_t sec = wall_ms / 1000u;
    const uint64_t usec = (wall_ms % 1000u) * 1000u;
    const unsigned id = (unsigned)(r->id & 0x1FFFFFFFu);
    const bool ext = (r->flags & LINSIGHT_REC_FLAG_EXT) != 0;
    int used;

    if (ext) {
        used = snprintf(buf, cap, "(%llu.%06llu) can0 %08X#",
                        (unsigned long long)sec, (unsigned long long)usec, id);
    } else {
        used = snprintf(buf, cap, "(%llu.%06llu) can0 %03X#",
                        (unsigned long long)sec, (unsigned long long)usec, id);
    }
    if (used < 0 || (size_t)used >= cap) {
        return -1;
    }
    if (r->flags & LINSIGHT_REC_FLAG_RTR) {
        if ((size_t)used + 2 > cap) {
            return -1;
        }
        buf[used++] = 'R';
        buf[used] = '\0';
        return used;
    }
    for (uint32_t i = 0; i < r->dlc && i < LINSIGHT_DLC_MAX; i++) {
        if ((size_t)used + 3 > cap) {
            return -1;
        }
        const int n = snprintf(buf + used, cap - (size_t)used, "%02X", r->data[i]);
        if (n != 2) {
            return -1;
        }
        used += n;
    }
    return used;
}

int canlog_format_candump(const linsight_can_record_t *r, uint64_t start_wall, char *buf,
                          size_t cap)
{
    return canlog_format_candump_ms(r, start_wall * 1000u, buf, cap);
}
