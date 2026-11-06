#include "canlog.h"

#include <errno.h>
#include <string.h>

static const char LSC_MAGIC[4] = {'L', 'S', 'C', '1'};

static int lsc_read_header(FILE *f, uint32_t *bitrate, uint64_t *start_wall)
{
    uint8_t hdr[LSC_HEADER_SIZE];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, LSC_MAGIC, 4) != 0 ||
        linsight_get_u16(hdr + 4) != LSC_VERSION) {
        errno = EPROTO;
        return -1;
    }
    if (bitrate) {
        *bitrate = linsight_get_u32(hdr + 6);
    }
    if (start_wall) {
        uint64_t t = 0;
        for (int i = 0; i < 8; i++) {
            t |= (uint64_t)hdr[16 + i] << (8 * i);
        }
        *start_wall = t;
    }
    return 0;
}

int lsc_writer_open(lsc_writer_t *w, const char *path, uint32_t bitrate, uint64_t start_wall)
{
    memset(w, 0, sizeof(*w));
    w->f = fopen(path, "wb");
    if (!w->f) {
        return -1;
    }

    uint8_t hdr[LSC_HEADER_SIZE] = {0};
    memcpy(hdr, LSC_MAGIC, 4);
    hdr[4] = LSC_VERSION & 0xFFu;
    hdr[5] = (LSC_VERSION >> 8) & 0xFFu;
    linsight_put_u32(hdr + 6, bitrate);
    linsight_put_u32(hdr + 10, 0); /* flags */
    for (int i = 0; i < 8; i++) {
        hdr[16 + i] = (uint8_t)(start_wall >> (8 * i));
    }

    w->bitrate = bitrate;
    w->start_wall = start_wall;
    if (fwrite(hdr, 1, LSC_HEADER_SIZE, w->f) != LSC_HEADER_SIZE) {
        fclose(w->f);
        w->f = NULL;
        return -1;
    }
    return 0;
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
    if (lsc_read_header(r->f, &r->bitrate, &r->start_wall) != 0) {
        fclose(r->f);
        r->f = NULL;
        return -1;
    }
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
    uint64_t start_wall;
    uint32_t bitrate;
    const lsc_filter_t *filter;
    long count;
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
    const uint64_t wall = e->start_wall + (uint64_t)(r->ts_ms / 1000u);
    if (f->from_valid && wall < f->from_wall) {
        return false;
    }
    if (f->to_valid && wall >= f->to_wall) {
        return false;
    }
    return true;
}

/* Vector-ASC-ish: "<wallclock> <bitrate-k>  <ID>[x] Rx   d|r <dlc> <data...>" */
static void export_asc_cb(const linsight_can_record_t *r, void *ctx)
{
    export_ctx_t *e = ctx;
    if (!export_match(r, e)) {
        return;
    }
    const double ts = (double)e->start_wall + (double)r->ts_ms / 1000.0;
    const char ext = (r->flags & LINSIGHT_REC_FLAG_EXT) ? 'x' : ' ';
    const char type = (r->flags & LINSIGHT_REC_FLAG_RTR) ? 'r' : 'd';

    if (r->flags & LINSIGHT_REC_FLAG_EXT) {
        fprintf(e->out, "%.6f %u  %08X%c Rx   %c %u", ts, (unsigned)e->bitrate / 1000u,
                (unsigned)r->id, ext, type, (unsigned)r->dlc);
    } else {
        fprintf(e->out, "%.6f %u    %03X%c Rx   %c %u", ts, (unsigned)e->bitrate / 1000u,
                (unsigned)r->id, ext, type, (unsigned)r->dlc);
    }
    for (int i = 0; i < r->dlc; i++) {
        fprintf(e->out, " %02X", r->data[i]);
    }
    fputc('\n', e->out);
    e->count++;
}

static void export_candump_cb(const linsight_can_record_t *r, void *ctx)
{
    export_ctx_t *e = ctx;
    if (!export_match(r, e)) {
        return;
    }
    const double ts = (double)e->start_wall + (double)r->ts_ms / 1000.0;

    fprintf(e->out, "(%.6f) can0 %s%08X", ts, (r->flags & LINSIGHT_REC_FLAG_EXT) ? "" : "   ",
            (unsigned)r->id);
    if (r->flags & LINSIGHT_REC_FLAG_RTR) {
        fprintf(e->out, " [R] ");
        for (int i = 0; i < r->dlc; i++) {
            fprintf(e->out, " %02X", r->data[i]);
        }
    } else {
        fprintf(e->out, " [%u]", (unsigned)r->dlc);
        for (int i = 0; i < r->dlc; i++) {
            fprintf(e->out, " %02X", r->data[i]);
        }
    }
    fputc('\n', e->out);
    e->count++;
}

static long export_common(const char *lsc_path, const char *out_path, lsc_record_cb cb,
                          const lsc_filter_t *filter)
{
    lsc_reader_t rd;
    if (lsc_reader_open(&rd, lsc_path) != 0) {
        return -1;
    }

    export_ctx_t e = {
        .start_wall = rd.start_wall,
        .bitrate = rd.bitrate,
        .filter = filter,
    };
    e.out = (strcmp(out_path, "-") == 0) ? stdout : fopen(out_path, "w");
    if (!e.out) {
        lsc_reader_close(&rd);
        return -1;
    }

    linsight_can_record_t rec;
    int rc;
    while ((rc = lsc_reader_next(&rd, &rec)) == 1) {
        cb(&rec, &e);
    }
    lsc_reader_close(&rd);
    if (e.out != stdout) {
        fclose(e.out);
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

int canlog_format_candump(const linsight_can_record_t *r, uint64_t start_wall, char *buf,
                          size_t cap)
{
    const double ts = (double)start_wall + (double)r->ts_ms / 1000.0;
    if (r->flags & LINSIGHT_REC_FLAG_RTR) {
        const size_t used =
            (size_t)snprintf(buf, cap, "(%.6f) can0 %s%08X remote request (%u)", ts,
                             (r->flags & LINSIGHT_REC_FLAG_EXT) ? "" : "   ",
                             (unsigned)r->id, (unsigned)r->dlc);
        return used >= cap ? -1 : (int)used;
    }
    size_t used = (size_t)snprintf(buf, cap, "(%.6f) can0 %s%08X [%u]", ts,
                                   (r->flags & LINSIGHT_REC_FLAG_EXT) ? "" : "   ",
                                   (unsigned)r->id, (unsigned)r->dlc);
    if (used >= cap) {
        return -1;
    }
    for (int i = 0; i < r->dlc && used + 3 < cap; i++) {
        used += (size_t)snprintf(buf + used, cap - used, " %02X", r->data[i]);
    }
    return (int)used;
}
