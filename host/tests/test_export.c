#include "check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "canlog.h"

static void make_record(linsight_can_record_t *r, uint16_t seq, uint32_t ts, uint32_t id,
                        uint8_t flags, uint8_t dlc)
{
    memset(r, 0, sizeof(*r));
    r->seq = seq;
    r->ts_ms = ts;
    r->id = id;
    r->flags = flags;
    r->dlc = dlc;
    for (uint8_t i = 0; i < dlc; i++) {
        r->data[i] = (uint8_t)(0x11u * (i + 1));
    }
}

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

int main(void)
{
    char path[] = "/tmp/linsight-export-XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);

    lsc_writer_t w;
    const lsc_open_info_t info = {
        .start_wall_ms = 1700000000000ull,
        .flags = LSC_FLAG_ANCHOR_MS_VALID | LSC_FLAG_BOOT_VALID,
        .boot_count = 7,
    };
    CHECK(lsc_writer_open_ex(&w, path, 500000, &info) == 0);

    linsight_can_record_t r;
    make_record(&r, 1, 1500, 0x123, 0, 2);
    CHECK(lsc_writer_write(&w, &r) == 0);
    make_record(&r, 2, 1600, 0x18DAF110u, LINSIGHT_REC_FLAG_EXT, 2);
    CHECK(lsc_writer_write(&w, &r) == 0);
    make_record(&r, 3, 1700, 0x321, LINSIGHT_REC_FLAG_RTR, 3);
    CHECK(lsc_writer_write(&w, &r) == 0);
    lsc_writer_close(&w);

    /* Reader exposes the extended header. */
    lsc_reader_t rd;
    CHECK(lsc_reader_open(&rd, path) == 0);
    CHECK(rd.start_wall_ms == 1700000000000ull);
    CHECK(rd.boot_count == 7);
    CHECK(rd.flags & LSC_FLAG_ANCHOR_MS_VALID);
    lsc_reader_close(&rd);

    char dump[] = "/tmp/linsight-export-dump-XXXXXX";
    const int dfd = mkstemp(dump);
    CHECK(dfd >= 0);
    close(dfd);
    CHECK(canlog_export_candump_filtered(path, dump, NULL) == 3);
    char *text = read_file(dump);
    CHECK(strstr(text, "(1700000001.500000) can0 123#1122\n") != NULL);
    CHECK(strstr(text, "(1700000001.600000) can0 18DAF110#1122\n") != NULL);
    CHECK(strstr(text, "(1700000001.700000) can0 321#R\n") != NULL);
    free(text);

    char asc[] = "/tmp/linsight-export-asc-XXXXXX";
    const int afd = mkstemp(asc);
    CHECK(afd >= 0);
    close(afd);
    CHECK(canlog_export_asc_filtered(path, asc, NULL) == 3);
    text = read_file(asc);
    CHECK(strstr(text, "base hex  timestamps absolute\n") != NULL);
    CHECK(strstr(text, "Begin Triggerblock ") != NULL);
    CHECK(strstr(text, "  1.500000 1  123            Rx   d 2 11 22\n") != NULL);
    CHECK(strstr(text, "  1.600000 1  18DAF110x") != NULL);
    CHECK(strstr(text, "  1.700000 1  321            Rx   r 3\n") != NULL);
    CHECK(strstr(text, "End Triggerblock\n") != NULL);
    free(text);

    char csv[] = "/tmp/linsight-export-csv-XXXXXX";
    const int cfd = mkstemp(csv);
    CHECK(cfd >= 0);
    close(cfd);
    CHECK(canlog_export_csv_filtered(path, csv, NULL) == 3);
    text = read_file(csv);
    CHECK(strstr(text, "wall_ms,uptime_ms,seq,id,ext,rtr,dlc,data_hex\n") != NULL);
    CHECK(strstr(text, "1700000001500,1500,1,0x123,0,0,2,1122\n") != NULL);
    CHECK(strstr(text, "1700000001600,1600,2,0x18DAF110,1,0,2,1122\n") != NULL);
    CHECK(strstr(text, "1700000001700,1700,3,0x321,0,1,3,\n") != NULL);
    free(text);

    /* Refuse to overwrite the input file. */
    CHECK(canlog_export_candump_filtered(path, path, NULL) == -1);
    CHECK(canlog_export_csv_filtered(path, path, NULL) == -1);

    unlink(path);
    unlink(dump);
    unlink(asc);
    unlink(csv);
    puts("export: ok");
    return 0;
}
