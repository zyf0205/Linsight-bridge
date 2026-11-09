#include "check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "canlog.h"
#include "import.h"

static void writer_make(const char *path)
{
    lsc_writer_t w;
    const lsc_open_info_t info = {
        .start_wall_ms = 0,
        .flags = LSC_FLAG_TS_UPTIME_ONLY | LSC_FLAG_BOOT_VALID,
        .boot_count = 42,
    };
    CHECK(lsc_writer_open_ex(&w, path, 500000, &info) == 0);
    linsight_can_record_t r;
    memset(&r, 0, sizeof(r));
    r.id = 0x123;
    r.dlc = 2;
    r.data[0] = 0x11;
    r.data[1] = 0x22;
    for (uint16_t i = 0; i < 3; i++) {
        r.seq = i;
        r.ts_ms = 1000 + i * 100;
        CHECK(lsc_writer_write(&w, &r) == 0);
    }
    CHECK(lsc_writer_close(&w) == 0);
}

int main(void)
{
    char src[] = "/tmp/linsight-import-src-XXXXXX";
    char dst[] = "/tmp/linsight-import-dst-XXXXXX";
    const int sfd = mkstemp(src);
    const int dfd = mkstemp(dst);
    CHECK(sfd >= 0 && dfd >= 0);
    close(sfd);
    close(dfd);

    writer_make(src);

    lsc_import_info_t info;
    char err[256] = {0};
    CHECK(lsc_verify(src, true, &info, err, sizeof(err)) == 0);
    CHECK(info.records == 3);
    CHECK(info.uptime_only);
    CHECK(info.boot_count == 42);
    CHECK(!info.truncated);

    CHECK(lsc_import(src, dst, true, 1700000000ull, &info, err, sizeof(err)) == 0);
    CHECK(info.records == 3);
    CHECK(info.start_wall_ms == 1700000000000ull);

    lsc_reader_t rd;
    CHECK(lsc_reader_open(&rd, dst) == 0);
    CHECK(rd.wall_valid);
    CHECK(rd.start_wall_ms == 1700000000000ull);
    CHECK(!(rd.flags & LSC_FLAG_TS_UPTIME_ONLY));
    CHECK(rd.flags & LSC_FLAG_ANCHOR_MS_VALID);
    CHECK(rd.boot_count == 42);
    lsc_reader_close(&rd);

    /* mtime anchoring: mtime 1700000100, last ts 1200 -> start 1699999.8 s. */
    struct utimbuf ut = {.actime = 1700000100, .modtime = 1700000100};
    CHECK(utime(src, &ut) == 0);
    CHECK(lsc_import(src, dst, false, 0, &info, err, sizeof(err)) == 0);
    CHECK(lsc_reader_open(&rd, dst) == 0);
    CHECK(rd.start_wall_ms == 1700000100000ull - 1200ull);
    lsc_reader_close(&rd);

    /* A truncated tail is tolerated for reading but fails --strict. */
    struct stat st;
    CHECK(stat(dst, &st) == 0);
    CHECK(truncate(dst, st.st_size - 7) == 0);
    const int vrc = lsc_verify(dst, true, &info, err, sizeof(err));
    CHECK(vrc == 1);
    CHECK(info.truncated);

    /* Refuse to overwrite the input. */
    CHECK(lsc_import(src, src, true, 1, &info, err, sizeof(err)) == -1);

    /* Buffered writes can succeed even when the final flush fails. */
    CHECK(lsc_import(src, "/dev/full", true, 1700000000ull, &info, err,
                     sizeof(err)) == -1);
    CHECK(strstr(err, "close failed") != NULL || strstr(err, "write failed") != NULL ||
          strstr(err, "cannot create") != NULL);

    /* Sidecars are summarized, not required. */
    char met[600];
    char trg[600];
    snprintf(met, sizeof(met), "%s.met", src);
    snprintf(trg, sizeof(trg), "%s.trg", src);
    FILE *f = fopen(met, "w");
    CHECK(f);
    fputs("{\"device_id\":\"AABB\",\"boot\":42,\"closed\":true}\n", f);
    fclose(f);
    f = fopen(trg, "w");
    CHECK(f);
    fputs("{\"reason\":\"host\"}\n{\"reason\":\"ui\"}\n", f);
    fclose(f);
    FILE *out = tmpfile();
    CHECK(out);
    lsc_print_sidecars(src, out);
    fflush(out);
    rewind(out);
    char buf[512] = {0};
    CHECK(fread(buf, 1, sizeof(buf) - 1, out) > 0);
    fclose(out);
    CHECK(strstr(buf, "device_id") != NULL);
    CHECK(strstr(buf, "trg: 2 mark(s)") != NULL);

    unlink(src);
    unlink(dst);
    unlink(met);
    unlink(trg);
    puts("import: ok");
    return 0;
}
