/* FIFO frame matching for `linsight-cli compare`. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "canlog.h"
#include "compare.h"

static int g_fail;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

static char g_ref[64];
static char g_cand[64];

static void write_log(const char *path, const linsight_can_record_t *recs, size_t n)
{
    lsc_writer_t w;
    CHECK(lsc_writer_open(&w, path, 500000, 1700000000u) == 0);
    for (size_t i = 0; i < n; i++) {
        CHECK(lsc_writer_write(&w, &recs[i]) == 0);
    }
    lsc_writer_close(&w);
}

static const linsight_can_record_t BASE[3] = {
    {.id = 0x100, .dlc = 1, .data = {0x11}},
    {.id = 0x101, .dlc = 2, .data = {0x22, 0x33}},
    {.id = 0x102, .flags = LINSIGHT_REC_FLAG_EXT, .dlc = 0},
};

static void expect(const linsight_can_record_t *ref, size_t nref,
                   const linsight_can_record_t *cand, size_t ncand, compare_stats_t want)
{
    write_log(g_ref, ref, nref);
    write_log(g_cand, cand, ncand);
    compare_stats_t st;
    CHECK(canlog_compare(g_ref, g_cand, &st) == 0);
    CHECK(st.ref_frames == want.ref_frames);
    CHECK(st.cand_frames == want.cand_frames);
    CHECK(st.matched == want.matched);
    CHECK(st.missing == want.missing);
    CHECK(st.extra == want.extra);
    CHECK(st.order == want.order);
}

int main(void)
{
    snprintf(g_ref, sizeof(g_ref), "/tmp/test-compare-ref-XXXXXX");
    const int rfd = mkstemp(g_ref);
    if (rfd < 0) {
        perror("mkstemp");
        return 1;
    }
    close(rfd);
    snprintf(g_cand, sizeof(g_cand), "/tmp/test-compare-cand-XXXXXX");
    const int cfd = mkstemp(g_cand);
    if (cfd < 0) {
        perror("mkstemp");
        return 1;
    }
    close(cfd);

    /* Identical recordings. */
    expect(BASE, 3, BASE, 3,
           (compare_stats_t){.ref_frames = 3, .cand_frames = 3, .matched = 3});

    /* One frame missing in the candidate. */
    const linsight_can_record_t miss[2] = {BASE[0], BASE[2]};
    expect(BASE, 3, miss, 2,
           (compare_stats_t){.ref_frames = 3, .cand_frames = 2, .matched = 2,
                             .missing = 1});

    /* Duplicate frame in the candidate. */
    const linsight_can_record_t dup[4] = {BASE[0], BASE[1], BASE[2], BASE[0]};
    expect(BASE, 3, dup, 4,
           (compare_stats_t){.ref_frames = 3, .cand_frames = 4, .matched = 3,
                             .extra = 1});

    /* Same frames, first two swapped. */
    const linsight_can_record_t swapped[3] = {BASE[1], BASE[0], BASE[2]};
    expect(BASE, 3, swapped, 3,
           (compare_stats_t){.ref_frames = 3, .cand_frames = 3, .matched = 3,
                             .order = 1});

    /* One payload byte differs: one missing and one extra frame. */
    linsight_can_record_t changed[3];
    memcpy(changed, BASE, sizeof(changed));
    changed[1].data[0] = 0xFF;
    expect(BASE, 3, changed, 3,
           (compare_stats_t){.ref_frames = 3, .cand_frames = 3, .matched = 2,
                             .missing = 1, .extra = 1});

    /* Empty reference / empty candidate. */
    expect(NULL, 0, BASE, 3,
           (compare_stats_t){.ref_frames = 0, .cand_frames = 3, .extra = 3});
    expect(BASE, 3, NULL, 0,
           (compare_stats_t){.ref_frames = 3, .cand_frames = 0, .missing = 3});

    /* Repeated identical frames match FIFO one-for-one. */
    const linsight_can_record_t twice[2] = {BASE[0], BASE[0]};
    expect(twice, 2, twice, 2,
           (compare_stats_t){.ref_frames = 2, .cand_frames = 2, .matched = 2});

    unlink(g_ref);
    unlink(g_cand);
    if (g_fail) {
        fprintf(stderr, "test_compare: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_compare: all passed\n");
    return 0;
}
