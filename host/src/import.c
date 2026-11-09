#include "import.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "canlog.h"
#include "session.h"

static void sidecar_path(const char *lsc, const char *ext, char *out, size_t cap)
{
    size_t n = strlen(lsc);
    if (n > 4 && strcmp(lsc + n - 4, ".lsc") == 0) {
        n -= 4;
    }
    snprintf(out, cap, "%.*s.%s", (int)n, lsc, ext);
}

static bool file_truncated(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < (off_t)LSC_HEADER_SIZE) {
        return true;
    }
    return ((uint64_t)st.st_size - LSC_HEADER_SIZE) % LINSIGHT_REC_SIZE != 0;
}

static int scan_last_ts(const char *path, uint32_t *last_ts)
{
    lsc_reader_t rd;
    if (lsc_reader_open(&rd, path) != 0) {
        return -1;
    }
    linsight_can_record_t rec;
    int rc;
    *last_ts = 0;
    while ((rc = lsc_reader_next(&rd, &rec)) == 1) {
        *last_ts = rec.ts_ms;
    }
    lsc_reader_close(&rd);
    return rc < 0 ? -1 : 0;
}

int lsc_import(const char *src, const char *dst, bool have_boot_wall,
               uint64_t boot_wall_sec, lsc_import_info_t *out, char *err, size_t errcap)
{
    if (!src || !dst || strcmp(src, dst) == 0) {
        snprintf(err, errcap, "source and destination must differ");
        return -1;
    }

    lsc_reader_t rd;
    if (lsc_reader_open(&rd, src) != 0) {
        snprintf(err, errcap, "cannot read %s: %s", src, strerror(errno));
        return -1;
    }

    lsc_import_info_t info;
    memset(&info, 0, sizeof(info));
    info.bitrate = rd.bitrate;
    info.boot_count = rd.boot_count;
    info.flags = rd.flags;
    info.uptime_only = !rd.wall_valid;
    info.truncated = file_truncated(src);

    uint64_t start_wall_ms = rd.start_wall_ms;
    if (!rd.wall_valid) {
        if (have_boot_wall) {
            start_wall_ms = boot_wall_sec * 1000u;
        } else {
            uint32_t last_ts = 0;
            if (scan_last_ts(src, &last_ts) != 0) {
                lsc_reader_close(&rd);
                snprintf(err, errcap, "cannot rescan %s", src);
                return -1;
            }
            struct stat st;
            if (stat(src, &st) != 0) {
                lsc_reader_close(&rd);
                snprintf(err, errcap, "stat %s: %s", src, strerror(errno));
                return -1;
            }
            const uint64_t mtime_ms = (uint64_t)st.st_mtime * 1000u;
            start_wall_ms = mtime_ms > last_ts ? mtime_ms - last_ts : 0;
        }
        info.flags &= ~LSC_FLAG_TS_UPTIME_ONLY;
        info.flags |= LSC_FLAG_ANCHOR_MS_VALID;
    } else {
        info.flags |= LSC_FLAG_ANCHOR_MS_VALID;
    }

    const lsc_open_info_t open_info = {
        .start_wall_ms = start_wall_ms,
        .flags = info.flags,
        .boot_count = info.boot_count,
    };
    lsc_writer_t w;
    if (lsc_writer_open_ex(&w, dst, info.bitrate, &open_info) != 0) {
        lsc_reader_close(&rd);
        snprintf(err, errcap, "cannot create %s: %s", dst, strerror(errno));
        return -1;
    }

    linsight_can_record_t rec;
    int rc;
    while ((rc = lsc_reader_next(&rd, &rec)) == 1) {
        if (lsc_writer_write(&w, &rec) != 0) {
            lsc_writer_close(&w);
            lsc_reader_close(&rd);
            snprintf(err, errcap, "write failed: %s", strerror(errno));
            return -1;
        }
        info.records++;
    }
    lsc_writer_close(&w);
    lsc_reader_close(&rd);
    if (rc < 0) {
        snprintf(err, errcap, "read failed in %s", src);
        return -1;
    }

    info.start_wall_ms = start_wall_ms;
    if (out) {
        *out = info;
    }
    return 0;
}

int lsc_verify(const char *path, bool strict, lsc_import_info_t *out, char *err,
               size_t errcap)
{
    lsc_reader_t rd;
    if (lsc_reader_open(&rd, path) != 0) {
        snprintf(err, errcap, "bad header in %s: %s", path, strerror(errno));
        return 2;
    }

    lsc_import_info_t info;
    memset(&info, 0, sizeof(info));
    info.bitrate = rd.bitrate;
    info.boot_count = rd.boot_count;
    info.flags = rd.flags;
    info.start_wall_ms = rd.start_wall_ms;
    info.uptime_only = !rd.wall_valid;
    info.truncated = file_truncated(path);

    session_state_t s;
    memset(&s, 0, sizeof(s));
    session_reset(&s, rd.boot_count, (rd.flags & LSC_FLAG_BOOT_VALID) != 0);

    linsight_can_record_t rec;
    bool have_ts = false;
    uint32_t last_ts = 0;
    int rc;
    while ((rc = lsc_reader_next(&rd, &rec)) == 1) {
        if (have_ts && rec.ts_ms < last_ts) {
            info.backwards++;
        }
        last_ts = rec.ts_ms;
        have_ts = true;
        (void)session_record_new(&s, &rec);
        info.records++;
    }
    lsc_reader_close(&rd);

    info.gaps = s.gaps;
    info.missing = s.missing;
    info.duplicates = s.skipped;
    info.ambiguous = s.ambiguous;
    if (out) {
        *out = info;
    }
    if (rc < 0) {
        snprintf(err, errcap, "read error in %s", path);
        return 2;
    }
    if (info.truncated) {
        if (strict) {
            snprintf(err, errcap, "truncated tail record");
            return 1;
        }
    }
    if (strict && (info.gaps || info.missing || info.backwards || info.duplicates)) {
        snprintf(err, errcap, "sequence/time defects (gaps=%llu missing=%llu backwards=%llu dup=%llu)",
                 (unsigned long long)info.gaps, (unsigned long long)info.missing,
                 (unsigned long long)info.backwards,
                 (unsigned long long)info.duplicates);
        return 1;
    }
    return 0;
}

static void print_sidecar(const char *path, FILE *out)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[512];
    if (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        fprintf(out, "met: %s\n", line);
    }
    fclose(f);
}

void lsc_print_sidecars(const char *lsc_path, FILE *out)
{
    char path[600];
    sidecar_path(lsc_path, "met", path, sizeof(path));
    print_sidecar(path, out);

    sidecar_path(lsc_path, "trg", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    unsigned lines = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        lines++;
    }
    fclose(f);
    fprintf(out, "trg: %u mark(s)\n", lines);
}
