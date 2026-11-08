/* test_sim.c — simulator unit tests.
 *
 * Covers pattern parsing, rate clamping, tick generation counting, inject
 * validation, and the can_soak.py-compatible counter layout. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "frame.h"
#include "sim.h"

static int g_emitted;
static linsight_can_record_t g_last;

/* The counter pattern reads the daemon-lifetime frame count and seq; expose
 * them so the test does not hard-code numbers. */
uint32_t sim_snapshot_generated(void);
uint16_t sim_snapshot_seq(void);

static void test_emit(void *ctx, const linsight_can_record_t *recs, uint32_t n)
{
    (void)ctx;
    g_emitted += (int)n;
    if (n) {
        g_last = recs[n - 1];
    }
}

static void reset_sim(void)
{
    sim_stop();
    g_emitted = 0;
    memset(&g_last, 0, sizeof(g_last));
}

int main(void)
{
    /* --- pattern parsing --- */
    sim_pattern_t p;
    assert(sim_pattern_parse("engine", &p) && p == SIM_PATTERN_ENGINE);
    assert(sim_pattern_parse("bms", &p) && p == SIM_PATTERN_BMS);
    assert(sim_pattern_parse("mix", &p) && p == SIM_PATTERN_MIX);
    assert(sim_pattern_parse("counter", &p) && p == SIM_PATTERN_COUNTER);
    assert(!sim_pattern_parse("nope", &p));
    assert(!sim_pattern_parse("", &p));
    assert(!sim_pattern_parse(NULL, &p));
    assert(strcmp(sim_pattern_name(SIM_PATTERN_ENGINE), "engine") == 0);
    assert(strcmp(sim_pattern_name(SIM_PATTERN_NONE), "none") == 0);

    /* --- start / stop lifecycle --- */
    reset_sim();
    assert(sim_start("mix", 10, test_emit, NULL));
    /* start emits once immediately */
    assert(g_emitted == 1);
    /* double start rejected */
    assert(!sim_start("engine", 10, test_emit, NULL));
    sim_status_t st;
    sim_status(&st);
    assert(st.running && st.pattern == SIM_PATTERN_MIX && st.rate_fps == 10);
    assert(strcmp(st.last_error, "already running") == 0);

    /* --- rate clamping --- */
    sim_stop();
    reset_sim();
    assert(sim_start("engine", 0, test_emit, NULL)); /* clamps to 1 */
    sim_status(&st);
    assert(st.rate_fps == 1);
    sim_stop();
    assert(sim_start("engine", 99999, test_emit, NULL)); /* clamps to 1000 */
    sim_status(&st);
    assert(st.rate_fps == 1000);
    sim_stop();
    assert(!sim_start("bogus", 10, test_emit, NULL));

    /* --- tick generation: 50 fps over 10 ticks = ~50 frames --- */
    reset_sim();
    assert(sim_start("counter", 50, test_emit, NULL));
    const int immediate = g_emitted;
    const uint16_t first_seq = g_last.seq;
    for (int i = 0; i < 10; i++) {
        sim_tick(test_emit, NULL);
    }
    /* 1 immediate + 10 ticks * 5 frames = 51; tolerate scheduler jitter of
     * zero here because ticks are driven manually. */
    assert(g_emitted == immediate + 50);
    /* seq must advance frame by frame, exactly like the device: `verify`
     * counts a repeated seq as a duplicate. */
    assert(g_last.seq == (uint16_t)(first_seq + 50));
    sim_stop();

    /* --- counter layout matches can_soak.py: u32 LE + 11 22 33 44 on 0x123.
     * The counter is the daemon-lifetime frame count (never reset), like a
     * real generator: it only has to be strictly monotone across frames. */
    reset_sim();
    assert(sim_start("counter", 10, test_emit, NULL));
    assert(g_last.id == 0x123u);
    assert(g_last.dlc == 8);
    uint32_t ctr;
    memcpy(&ctr, g_last.data, 4); /* little-endian host assumption is fine on x86 */
    assert(ctr == sim_snapshot_generated() - 1u); /* last frame carries n-1 */
    assert(g_last.data[4] == 0x11 && g_last.data[5] == 0x22 && g_last.data[6] == 0x33 &&
           g_last.data[7] == 0x44);
    const uint32_t prev = ctr;
    sim_tick(test_emit, NULL);
    memcpy(&ctr, g_last.data, 4);
    assert(ctr == prev + 1u); /* strictly monotone */
    sim_stop();

    /* --- engine pattern shape --- */
    reset_sim();
    assert(sim_start("engine", 10, test_emit, NULL));
    assert(g_last.id == 0x0C0u && g_last.dlc == 6 && g_last.flags == 0);
    sim_stop();

    /* --- inject --- */
    reset_sim();
    const uint8_t data[8] = {0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4};
    const uint16_t seq_before = sim_snapshot_seq();
    assert(sim_inject(0x1AB, false, false, 4, data, test_emit, NULL));
    assert(g_emitted == 1 && g_last.id == 0x1AB && g_last.dlc == 4);
    assert(g_last.seq == seq_before); /* inject consumes one seq too */
    assert(g_last.data[0] == 0xDE && g_last.data[3] == 0xEF);
    /* extended */
    assert(sim_inject(0x1ABCDEF, true, false, 8, data, test_emit, NULL));
    assert(g_last.id == 0x1ABCDEF && (g_last.flags & LINSIGHT_REC_FLAG_EXT));
    /* rtr with dlc only */
    assert(sim_inject(0x123, false, true, 8, NULL, test_emit, NULL));
    assert(g_last.flags & LINSIGHT_REC_FLAG_RTR);
    /* rejections */
    assert(!sim_inject(0x800, false, false, 0, NULL, test_emit, NULL)); /* std > 0x7FF */
    assert(!sim_inject(0x20000000, true, false, 0, NULL, test_emit, NULL)); /* ext max */
    assert(!sim_inject(0x1, false, false, 9, data, test_emit, NULL)); /* dlc > 8 */
    assert(!sim_inject(0x1, false, false, 1, NULL, test_emit, NULL)); /* null data */

    /* --- status after everything: generated counts all injects (3 ok) --- */
    const uint32_t before_injects = sim_snapshot_generated();
    sim_status(&st);
    assert(!st.running && st.generated == before_injects && before_injects >= 3u);
    assert(st.last_error[0] == '\0');

    puts("test_sim: all assertions passed");
    return 0;
}
