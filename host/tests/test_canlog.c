/* .lsc header validation, sequential reader, and export filters. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "canlog.h"

static int g_fail;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

static char g_path[64];
static char g_out[64];

static long count_lines(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    long lines = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            lines++;
        }
    }
    fclose(f);
    return lines;
}

static void write_sample(void)
{
    lsc_writer_t w;
    CHECK(lsc_writer_open(&w, g_path, 500000, 1700000000u) == 0);

    const linsight_can_record_t recs[] = {
        {.seq = 0, .ts_ms = 1000, .id = 0x123, .flags = 0, .dlc = 2, .data = {0x11, 0x22}},
        {.seq = 1,
         .ts_ms = 2500,
         .id = 0x1ABCDEF,
         .flags = LINSIGHT_REC_FLAG_EXT,
         .dlc = 1,
         .data = {0xAA}},
        {.seq = 2,
         .ts_ms = 4000,
         .id = 0x123,
         .flags = LINSIGHT_REC_FLAG_RTR,
         .dlc = 0},
    };
    for (size_t i = 0; i < sizeof(recs) / sizeof(recs[0]); i++) {
        CHECK(lsc_writer_write(&w, &recs[i]) == 0);
    }
    lsc_writer_close(&w);
}

static long export_with(const lsc_filter_t *filter)
{
    const long n = canlog_export_candump_filtered(g_path, g_out, filter);
    if (n < 0) {
        return n;
    }
    const long lines = count_lines(g_out);
    return lines == n ? n : -2;
}

static void test_reader_and_filters(void)
{
    lsc_reader_t r;
    CHECK(lsc_reader_open(&r, g_path) == 0);
    CHECK(r.bitrate == 500000);
    CHECK(r.start_wall == 1700000000u);

    linsight_can_record_t rec;
    CHECK(lsc_reader_next(&r, &rec) == 1 && rec.id == 0x123 && rec.dlc == 2);
    CHECK(lsc_reader_next(&r, &rec) == 1 && rec.id == 0x1ABCDEF);
    CHECK(lsc_reader_next(&r, &rec) == 1 && rec.id == 0x123);
    CHECK(lsc_reader_next(&r, &rec) == 0);
    lsc_reader_close(&r);

    lsc_filter_t std = {.ext = 0};
    CHECK(export_with(&std) == 2);
    lsc_filter_t ext = {.ext = 1};
    CHECK(export_with(&ext) == 1);
    lsc_filter_t id = {.id_valid = true, .id = 0x123, .ext = -1};
    CHECK(export_with(&id) == 2);
    lsc_filter_t window = {
        .ext = -1,
        .from_valid = true,
        .from_wall = 1700000001u,
        .to_valid = true,
        .to_wall = 1700000004u,
    };
    CHECK(export_with(&window) == 2);
}

static void corrupt_byte(size_t offset, unsigned char value)
{
    FILE *f = fopen(g_path, "r+b");
    CHECK(f != NULL);
    if (!f) {
        return;
    }
    CHECK(fseek(f, (long)offset, SEEK_SET) == 0);
    CHECK(fputc(value, f) == value);
    fclose(f);
}

static void test_rejects_bad_header(void)
{
    lsc_reader_t r;
    write_sample();
    corrupt_byte(4, 2); /* version */
    errno = 0;
    CHECK(lsc_reader_open(&r, g_path) == -1);
    CHECK(errno == EPROTO);

    write_sample();
    corrupt_byte(0, 'X'); /* magic */
    errno = 0;
    CHECK(lsc_reader_open(&r, g_path) == -1);
    CHECK(errno == EPROTO);

    /* A truncated tail record is tolerated, not treated as corruption. */
    write_sample();
    FILE *f = fopen(g_path, "ab");
    CHECK(f != NULL);
    if (!f) {
        return;
    }
    const unsigned char tail[7] = {1, 2, 3, 4, 5, 6, 7};
    CHECK(fwrite(tail, 1, sizeof(tail), f) == sizeof(tail));
    fclose(f);
    CHECK(lsc_reader_open(&r, g_path) == 0);
    int n = 0;
    linsight_can_record_t rec;
    while (lsc_reader_next(&r, &rec) == 1) {
        n++;
    }
    lsc_reader_close(&r);
    CHECK(n == 3);
}

static void test_reader_snapshot(void)
{
    write_sample();

    lsc_reader_t r;
    CHECK(lsc_reader_open(&r, g_path) == 0);

    /* Records appended after open must not be visible: replay relies on this
     * to stop at the snapshot while the daemon keeps writing the same file. */
    FILE *f = fopen(g_path, "ab");
    CHECK(f != NULL);
    if (!f) {
        lsc_reader_close(&r);
        return;
    }
    const linsight_can_record_t extra = {
        .seq = 3, .ts_ms = 5000, .id = 0x456, .flags = 0, .dlc = 1, .data = {0x55}};
    uint8_t buf[LINSIGHT_REC_SIZE];
    linsight_record_pack(&extra, buf);
    CHECK(fwrite(buf, 1, sizeof(buf), f) == sizeof(buf));
    fclose(f);

    int n = 0;
    linsight_can_record_t rec;
    while (lsc_reader_next(&r, &rec) == 1) {
        n++;
    }
    CHECK(n == 3);
    lsc_reader_close(&r);

    /* A reader opened afterwards sees the appended record. */
    CHECK(lsc_reader_open(&r, g_path) == 0);
    n = 0;
    while (lsc_reader_next(&r, &rec) == 1) {
        n++;
    }
    lsc_reader_close(&r);
    CHECK(n == 4);
}


static void test_header_extensions(void)
{
    lsc_writer_t w;
    const lsc_open_info_t info = {
        .start_wall_ms = 1700000000123ull,
        .flags = LSC_FLAG_ANCHOR_MS_VALID | LSC_FLAG_BOOT_VALID,
        .boot_count = 42,
    };
    CHECK(lsc_writer_open_ex(&w, g_path, 500000, &info) == 0);
    const linsight_can_record_t rec = {.seq = 0, .ts_ms = 5, .id = 0x123, .dlc = 0};
    CHECK(lsc_writer_write(&w, &rec) == 0);
    CHECK(lsc_writer_sync(&w) == 0);
    lsc_writer_close(&w);

    lsc_reader_t r;
    CHECK(lsc_reader_open(&r, g_path) == 0);
    CHECK(r.flags == (LSC_FLAG_ANCHOR_MS_VALID | LSC_FLAG_BOOT_VALID));
    CHECK(r.boot_count == 42);
    CHECK(r.anchor_ms == 123);
    CHECK(r.start_wall == 1700000000u);
    CHECK(r.start_wall_ms == 1700000000123ull);
    CHECK(r.wall_valid);
    lsc_reader_close(&r);

    /* Device SD-style header: uptime only, re-anchored later by import. */
    const lsc_open_info_t uptime = {
        .start_wall_ms = 0,
        .flags = LSC_FLAG_TS_UPTIME_ONLY | LSC_FLAG_BOOT_VALID,
        .boot_count = 7,
    };
    CHECK(lsc_writer_open_ex(&w, g_path, 500000, &uptime) == 0);
    lsc_writer_close(&w);
    CHECK(lsc_reader_open(&r, g_path) == 0);
    CHECK(!r.wall_valid);
    CHECK(r.boot_count == 7);
    CHECK(r.start_wall_ms == 0);
    lsc_reader_close(&r);
}

static void test_writer_errors(void)
{
    lsc_writer_t w = {0};
    CHECK(lsc_writer_flush(&w) == -1);
    CHECK(lsc_writer_sync(&w) == -1);
    CHECK(lsc_writer_close(&w) == 0);

    /* A buffered stream reports ENOSPC only when flushed/closed. */
    w.f = fopen("/dev/full", "wb");
    CHECK(w.f != NULL);
    if (!w.f) {
        return;
    }
    CHECK(fputs("pending", w.f) >= 0);
    CHECK(lsc_writer_close(&w) == -1);
    CHECK(w.f == NULL);

    /* fflush may later succeed, but a previous ferror remains a failure. */
    w.f = fopen("/dev/full", "wb");
    CHECK(w.f != NULL);
    if (!w.f) {
        return;
    }
    CHECK(setvbuf(w.f, NULL, _IONBF, 0) == 0);
    CHECK(fputc('x', w.f) == EOF);
    CHECK(ferror(w.f));
    CHECK(lsc_writer_flush(&w) == -1);
    CHECK(lsc_writer_sync(&w) == -1);
    CHECK(lsc_writer_close(&w) == -1);
    CHECK(w.f == NULL);

    /* Even a late fclose failure must clear the stream handle. */
    w.f = tmpfile();
    CHECK(w.f != NULL);
    if (!w.f) {
        return;
    }
    CHECK(close(fileno(w.f)) == 0);
    CHECK(lsc_writer_close(&w) == -1);
    CHECK(w.f == NULL);
}

int main(void)
{
    snprintf(g_path, sizeof(g_path), "/tmp/test-canlog-XXXXXX");
    const int fd = mkstemp(g_path);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    close(fd);
    snprintf(g_out, sizeof(g_out), "/tmp/test-canlog-out-XXXXXX");
    const int ofd = mkstemp(g_out);
    if (ofd < 0) {
        perror("mkstemp");
        return 1;
    }
    close(ofd);

    write_sample();
    test_reader_and_filters();
    test_rejects_bad_header();
    test_reader_snapshot();
    test_header_extensions();
    test_writer_errors();

    unlink(g_path);
    unlink(g_out);
    if (g_fail) {
        fprintf(stderr, "test_canlog: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_canlog: all passed\n");
    return 0;
}
