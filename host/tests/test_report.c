#include "check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "canlog.h"
#include "report.h"

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    CHECK(buf);
    CHECK(fread(buf, 1, (size_t)sz, f) == (size_t)sz);
    fclose(f);
    buf[sz] = '\0';
    return buf;
}

static void add(lsc_writer_t *w, uint16_t seq, uint32_t ts, uint32_t id, uint8_t flags)
{
    linsight_can_record_t r = {
        .seq = seq,
        .ts_ms = ts,
        .id = id,
        .flags = flags,
        .dlc = 2,
        .data = {0x11, 0x22},
    };
    CHECK(lsc_writer_write(w, &r) == 0);
}

int main(void)
{
    char path[] = "/tmp/linsight-report-XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);

    lsc_writer_t w;
    const lsc_open_info_t open_info = {
        .start_wall_ms = 1700000000000ull,
        .flags = LSC_FLAG_ANCHOR_MS_VALID | LSC_FLAG_BOOT_VALID,
        .boot_count = 3,
    };
    CHECK(lsc_writer_open_ex(&w, path, 500000, &open_info) == 0);
    for (uint16_t i = 0; i < 6; i++) {
        add(&w, i, i * 100u, 0x123, 0);
    }
    for (uint16_t i = 0; i < 4; i++) {
        add(&w, (uint16_t)(6 + i), 600u + i * 100u, 0x456, LINSIGHT_REC_FLAG_EXT);
    }
    CHECK(lsc_writer_close(&w) == 0);

    char html[] = "/tmp/linsight-report-html-XXXXXX";
    const int hfd = mkstemp(html);
    CHECK(hfd >= 0);
    close(hfd);

    lsc_report_info_t info = {0};
    CHECK(lsc_report_html(path, html, "Fleet bus", &info) == 0);
    CHECK(info.frames == 10);
    CHECK(info.unique_ids == 2);
    CHECK(info.start_ts_ms == 0);
    CHECK(info.end_ts_ms == 900);

    char *text = read_file(html);
    CHECK(strstr(text, "<title>Fleet bus</title>") != NULL);
    CHECK(strstr(text, "Top IDs") != NULL);
    CHECK(strstr(text, "0x123") != NULL);
    CHECK(strstr(text, "0x00000456 EXT") != NULL);
    CHECK(strstr(text, "500 kbit/s") != NULL);
    CHECK(strstr(text, "Frames</div>") != NULL);
    free(text);

    /* Refuse to overwrite the input log. */
    CHECK(lsc_report_html(path, path, NULL, NULL) == -1);

    /* An empty log still produces a readable page. */
    char empty[] = "/tmp/linsight-report-empty-XXXXXX";
    const int efd = mkstemp(empty);
    CHECK(efd >= 0);
    close(efd);
    CHECK(lsc_writer_open(&w, empty, 250000, 0) == 0);
    CHECK(lsc_writer_close(&w) == 0);
    char empty_html[] = "/tmp/linsight-report-empty-html-XXXXXX";
    const int ehfd = mkstemp(empty_html);
    CHECK(ehfd >= 0);
    close(ehfd);
    CHECK(lsc_report_html(empty, empty_html, NULL, NULL) == 0);
    text = read_file(empty_html);
    CHECK(strstr(text, "No records in this log.") != NULL);
    free(text);

    unlink(path);
    unlink(html);
    unlink(empty);
    unlink(empty_html);
    puts("report: ok");
    return 0;
}
