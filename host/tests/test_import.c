#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "canlog.h"
#include "import.h"

static void writer_make(const char *path, const char *lsc)
{
    lsc_writer_t w;
    const lsc_open_info_t info = {
        .start_wall_ms = 0,
        .flags = LSC_FLAG_TS_UPTIME_ONLY | LSC_FLAG_BOOT_VALID,
        .boot_count = 42,
    };
    assert(lsc_writer_open_ex(&w, path, 500000, &info) == 0);
    linsight_can_record_t r;
    memset(&r, 0, sizeof(r));
    r.id = 0x123;
    r.dlc = 2;
    r.data[0] = 0x11;
    r.data[1] = 0x22;
    for (uint16_t i = 0; i < 3; i++) {
        r.seq = i;
        r.ts_ms = 1000 + i * 100;
        assert(lsc_writer_write(&w, &r) == 0);
    }
    lsc_writer_close(&w);
    (void)lsc;
}

int main(void)
{
    char src[] = "/tmp/linsight-import-src-XXXXXX";
    char dst[] = "/tmp/linsight-import-dst-XXXXXX";
    const int sfd = mkstemp(src);
    const int dfd = mkstemp(dst);
    assert(sfd >= 0 && dfd >= 0);
    close(sfd);
    close(dfd);

    writer_make(src, NULL);

    lsc_import_info_t info;
    char err[256] = {0};
    assert(lsc_verify(src, true, &info, err, sizeof(err)) == 0);
    assert(info.records == 3);
    assert(info.uptime_only);
    assert(info.boot_count == 42);
    assert(!info.truncated);

    assert(lsc_import(src, dst, true, 1700000000ull, &info, err, sizeof(err)) == 0);
    assert(info.records == 3);
    assert(info.start_wall_ms == 1700000000000ull);

    lsc_reader_t rd;
    assert(lsc_reader_open(&rd, dst) == 0);
    assert(rd.wall_valid);
    assert(rd.start_wall_ms == 1700000000000ull);
    assert(!(rd.flags & LSC_FLAG_TS_UPTIME_ONLY));
    assert(rd.flags & LSC_FLAG_ANCHOR_MS_VALID);
    assert(rd.boot_count == 42);
    lsc_reader_close(&rd);

    /* mtime anchoring: mtime 1700000100, last ts 1200 -> start 1699999.8 s. */
    struct utimbuf ut = {.actime = 1700000100, .modtime = 1700000100};
    assert(utime(src, &ut) == 0);
    assert(lsc_import(src, dst, false, 0, &info, err, sizeof(err)) == 0);
    assert(lsc_reader_open(&rd, dst) == 0);
    assert(rd.start_wall_ms == 1700000100000ull - 1200ull);
    lsc_reader_close(&rd);

    /* A truncated tail is tolerated for reading but fails --strict. */
    struct stat st;
    assert(stat(dst, &st) == 0);
    assert(truncate(dst, st.st_size - 7) == 0);
    const int vrc = lsc_verify(dst, true, &info, err, sizeof(err));
    assert(vrc == 1);
    assert(info.truncated);

    /* Refuse to overwrite the input. */
    assert(lsc_import(src, src, true, 1, &info, err, sizeof(err)) == -1);

    /* Sidecars are summarized, not required. */
    char met[600];
    char trg[600];
    snprintf(met, sizeof(met), "%s.met", src);
    snprintf(trg, sizeof(trg), "%s.trg", src);
    FILE *f = fopen(met, "w");
    assert(f);
    fputs("{\"device_id\":\"AABB\",\"boot\":42,\"closed\":true}\n", f);
    fclose(f);
    f = fopen(trg, "w");
    assert(f);
    fputs("{\"reason\":\"host\"}\n{\"reason\":\"ui\"}\n", f);
    fclose(f);
    FILE *out = tmpfile();
    assert(out);
    lsc_print_sidecars(src, out);
    fflush(out);
    rewind(out);
    char buf[512] = {0};
    assert(fread(buf, 1, sizeof(buf) - 1, out) > 0);
    fclose(out);
    assert(strstr(buf, "device_id") != NULL);
    assert(strstr(buf, "trg: 2 mark(s)") != NULL);

    unlink(src);
    unlink(dst);
    unlink(met);
    unlink(trg);
    puts("import: ok");
    return 0;
}
