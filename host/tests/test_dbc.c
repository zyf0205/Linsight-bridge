#define _POSIX_C_SOURCE 200809L

#include "dbc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static const char kDbc[] =
    "VERSION \"\"\n"
    "\n"
    "NS_ :\n"
    "\n"
    "BU_: ECU\n"
    "\n"
    "BO_ 291 EngineData: 8 ECU\n"
    " SG_ Rpm : 0|16@1+ (0.25,0) [0|16383.75] \"rpm\" Vector__XXX\n"
    " SG_ Temp : 16|8@1- (1,-40) [-40|215] \"C\" Vector__XXX\n"
    " SG_ Flags : 24|8@1+ (1,0) [0|255] \"\" Vector__XXX\n"
    "\n"
    "BO_ 2147483939 ExtData: 8 ECU\n"
    " SG_ Motorola : 7|8@0+ (1,0) [0|255] \"\" Vector__XXX\n"
    " SG_ Mux M : 0|4@1+ (1,0) [0|15] \"\" Vector__XXX\n"
    " SG_ Muxed m1 : 8|8@1+ (1,0) [0|255] \"\" Vector__XXX\n";

int main(void)
{
    char path[] = "/tmp/linsight-dbc-XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0) {
        return 1;
    }
    FILE *f = fdopen(fd, "w");
    CHECK(f != NULL);
    if (!f) {
        close(fd);
        unlink(path);
        return 1;
    }
    fputs(kDbc, f);
    fclose(f);

    static dbc_t model;
    CHECK(dbc_load(&model, path) == 0);
    unlink(path);
    CHECK(model.count == 2);

    const dbc_message_t *std = dbc_find(&model, 0x123, 0);
    const dbc_message_t *ext = dbc_find(&model, 0x123, 1);
    CHECK(std != NULL);
    CHECK(ext != NULL);
    if (!std || !ext) {
        return 1;
    }

    CHECK(strcmp(std->name, "EngineData") == 0);
    CHECK(std->dlc == 8);
    CHECK(std->signal_count == 3); /* conditional m1 signal skipped */
    CHECK(strcmp(ext->name, "ExtData") == 0);
    CHECK(ext->signal_count == 2); /* Motorola + multiplexor M */

    const uint8_t data[8] = {0xB0, 0x04, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x00};
    const uint8_t ext_data[8] = {0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    double value = 0;

    CHECK(dbc_decode_signal(&std->signals[0], data, 8, &value) == 0);
    CHECK(fabs(value - 300.0) < 1e-9); /* 1200 * 0.25 */

    CHECK(dbc_decode_signal(&std->signals[1], data, 8, &value) == 0);
    CHECK(fabs(value - (-41.0)) < 1e-9); /* signed 0xFF -> -1, -40 offset */

    CHECK(dbc_decode_signal(&std->signals[2], data, 8, &value) == 0);
    CHECK(fabs(value - 127.0) < 1e-9);

    CHECK(dbc_decode_signal(&std->signals[0], data, 1, &value) == -1);

    CHECK(dbc_decode_signal(&ext->signals[0], ext_data, 8, &value) == 0);
    CHECK(fabs(value - 0x12) < 1e-9); /* Motorola 7|8 */

    CHECK(dbc_decode_signal(&ext->signals[1], ext_data, 8, &value) == 0);
    CHECK(fabs(value - 0x2) < 1e-9); /* low nibble */

    if (failures) {
        fprintf(stderr, "test_dbc: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_dbc: ok\n");
    return 0;
}
