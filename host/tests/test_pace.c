/*
 * Regression test for host/src/pace.c — the pacing queue of the bridge.
 *
 * All time is injected (now_ns / wall ms), so every check is deterministic:
 * wall anchoring, late-record compression, FIRST-mode base + speed scaling,
 * session re-basing, backwards timestamps, same-target batching, drop-oldest
 * overflow, the epoll timeout tri-state, pop-cap truncation and seq/ts wrap.
 */

#include "check.h"

#include <stdint.h>
#include <string.h>

#include "pace.h"

#define MS 1000000ULL
#define SEC 1000000000ULL

/* ts=500 maps onto the bind instant: mono 500 ms, wall WALL_NOW_MS. */
#define WALL_NOW_NS 500000000ULL
#define WALL_NOW_MS 1700000000000ULL
#define WALL_ANCHOR_MS (WALL_NOW_MS - 500ULL)

static pace_t g_p;

static linsight_can_record_t mkrec(uint16_t seq, uint32_t ts, uint32_t id)
{
    linsight_can_record_t r;
    memset(&r, 0, sizeof(r));
    r.seq = seq;
    r.ts_ms = ts;
    r.id = id;
    return r;
}

static void push(uint16_t seq, uint32_t ts, uint32_t id, uint64_t now_ns)
{
    linsight_can_record_t r = mkrec(seq, ts, id);
    pace_push(&g_p, &r, now_ns);
}

static void test_wall_anchor(void)
{
    pace_reset(&g_p, PACE_ANCHOR_WALL, 3.0); /* speed must be ignored */
    pace_bind_wall(&g_p, WALL_ANCHOR_MS, WALL_NOW_NS, WALL_NOW_MS);

    push(1, 10, 101u, WALL_NOW_NS);
    push(2, 20, 102u, WALL_NOW_NS);

    /* ts=10 -> mono 10 ms, ts=20 -> 20 ms (independent of speed). */
    CHECK(pace_count(&g_p) == 2);
    linsight_can_record_t out[4];
    CHECK(pace_pop_due(&g_p, 10 * MS - 1, out, 4) == 0);
    CHECK(pace_pop_due(&g_p, 10 * MS, out, 4) == 1);
    CHECK(out[0].id == 101u);
    CHECK(pace_count(&g_p) == 1);
    CHECK(pace_pop_due(&g_p, 20 * MS - 1, out, 4) == 0);
    CHECK(pace_pop_due(&g_p, 20 * MS, out, 4) == 1);
    CHECK(out[0].id == 102u);
    CHECK(pace_count(&g_p) == 0);
}

static void test_wall_late(void)
{
    /* A target already in the past pops immediately: reconnect backlog is
     * compressed instead of replayed with sleeps. */
    pace_reset(&g_p, PACE_ANCHOR_WALL, 1.0);
    pace_bind_wall(&g_p, WALL_ANCHOR_MS, WALL_NOW_NS, WALL_NOW_MS);
    push(1, 10, 201u, WALL_NOW_NS);
    push(2, 20, 202u, WALL_NOW_NS);
    linsight_can_record_t out[8];
    CHECK(pace_pop_due(&g_p, 5 * SEC, out, 8) == 2);
    CHECK(out[0].id == 201u && out[1].id == 202u);
}

static void test_first_base_speed(void)
{
    const uint64_t t0 = 1 * SEC;
    pace_reset(&g_p, PACE_ANCHOR_FIRST, 2.0);
    push(1, 100, 301u, t0); /* first record anchors base at now */
    push(2, 102, 302u, t0); /* +2 ms device / 2.0 -> +1 ms mono */
    CHECK(pace_count(&g_p) == 2);
    linsight_can_record_t out[4];
    CHECK(pace_pop_due(&g_p, t0, out, 4) == 1);
    CHECK(out[0].id == 301u);
    CHECK(pace_epoll_timeout_ms(&g_p, t0) == 1);
    CHECK(pace_pop_due(&g_p, t0 + MS, out, 4) == 1);
    CHECK(out[0].id == 302u);

    /* speed <= 0 behaves like 1.0 */
    pace_reset(&g_p, PACE_ANCHOR_FIRST, -1.0);
    push(1, 500, 303u, t0);
    push(2, 503, 304u, t0);
    CHECK(pace_pop_due(&g_p, t0, out, 4) == 1);
    CHECK(out[0].id == 303u);
    CHECK(pace_epoll_timeout_ms(&g_p, t0) == 3);
    CHECK(pace_pop_due(&g_p, t0 + 3 * MS, out, 4) == 1);
    CHECK(out[0].id == 304u);
}

static void test_new_session(void)
{
    const uint64_t t0 = 1 * SEC;
    pace_reset(&g_p, PACE_ANCHOR_FIRST, 1.0);
    push(1, 0, 401u, t0);    /* base t0 */
    push(2, 1000, 402u, t0); /* t0 + 1 s */

    /* Seam inside the old tail window: base = last_target + 1 ms. */
    pace_new_session(&g_p, t0 + 500 * MS);
    push(3, 0, 403u, t0 + 500 * MS); /* re-bases to t0 + 1001 ms */
    linsight_can_record_t out[8];
    CHECK(pace_pop_due(&g_p, t0 + MS, out, 8) == 1);
    CHECK(out[0].id == 401u);
    CHECK(pace_pop_due(&g_p, t0 + 1000 * MS, out, 8) == 1);
    CHECK(out[0].id == 402u);
    CHECK(pace_pop_due(&g_p, t0 + 1001 * MS - 1, out, 8) == 0);
    CHECK(pace_pop_due(&g_p, t0 + 1001 * MS, out, 8) == 1);
    CHECK(out[0].id == 403u);

    /* Seam after the queue drained: base = now. */
    pace_new_session(&g_p, t0 + 5 * SEC);
    push(4, 7, 404u, t0 + 5 * SEC); /* base t0 + 5 s */
    push(5, 9, 405u, t0 + 5 * SEC); /* +2 ms */
    CHECK(pace_epoll_timeout_ms(&g_p, t0 + 5 * SEC) == 0);
    CHECK(pace_pop_due(&g_p, t0 + 5 * SEC, out, 8) == 1);
    CHECK(out[0].id == 404u);
    CHECK(pace_epoll_timeout_ms(&g_p, t0 + 5 * SEC) == 2);
    CHECK(pace_pop_due(&g_p, t0 + 5 * SEC + 2 * MS, out, 8) == 1);
    CHECK(out[0].id == 405u);
}

static void test_ts_backwards(void)
{
    const uint64_t t0 = 1 * SEC;
    pace_reset(&g_p, PACE_ANCHOR_FIRST, 1.0);
    push(1, 500, 501u, t0); /* t0 */
    push(2, 700, 502u, t0); /* t0 + 200 ms */
    push(3, 100, 503u, t0); /* ts backwards: delta 0 -> previous target */
    CHECK(g_p.merged == 1);
    CHECK(g_p.dropped == 0);
    linsight_can_record_t out[8];
    CHECK(pace_pop_due(&g_p, t0, out, 8) == 1);
    CHECK(out[0].id == 501u);
    CHECK(pace_pop_due(&g_p, t0 + 200 * MS - 1, out, 8) == 0);
    CHECK(pace_pop_due(&g_p, t0 + 200 * MS, out, 8) == 2);
    CHECK(out[0].id == 502u && out[1].id == 503u);
}

static void test_same_ms_batch(void)
{
    pace_reset(&g_p, PACE_ANCHOR_WALL, 1.0);
    pace_bind_wall(&g_p, WALL_ANCHOR_MS, WALL_NOW_NS, WALL_NOW_MS);
    push(1, 77, 601u, WALL_NOW_NS);
    push(2, 77, 602u, WALL_NOW_NS);
    push(3, 77, 603u, WALL_NOW_NS);
    CHECK(g_p.merged == 2);
    CHECK(pace_count(&g_p) == 3);
    linsight_can_record_t out[8];
    CHECK(pace_pop_due(&g_p, 77 * MS - 1, out, 8) == 0);
    CHECK(pace_pop_due(&g_p, 77 * MS, out, 8) == 3);
    CHECK(out[0].id == 601u && out[1].id == 602u && out[2].id == 603u);
}

static void test_full_drop_oldest(void)
{
    const uint64_t t0 = 1 * SEC;
    pace_reset(&g_p, PACE_ANCHOR_FIRST, 1.0);
    for (uint32_t i = 0; i < PACEQ_CAP + 5u; i++) {
        push((uint16_t)i, i * 10u, i, t0); /* 10 ms apart: no batching */
    }
    CHECK(g_p.dropped == 5);
    CHECK(g_p.merged == 0);
    CHECK(pace_count(&g_p) == PACEQ_CAP);

    /* The 5 oldest entries (id 0..4) were dropped; the rest pop in order. */
    static linsight_can_record_t chunk[65536];
    const uint64_t t_end = t0 + (uint64_t)(PACEQ_CAP + 5u) * 10u * MS;
    uint32_t total = 0;
    uint32_t expect = 5;
    for (;;) {
        uint32_t n = pace_pop_due(&g_p, t_end, chunk, 65536);
        for (uint32_t i = 0; i < n; i++) {
            CHECK(chunk[i].id == expect);
            expect++;
        }
        total += n;
        if (n == 0) {
            break;
        }
    }
    CHECK(total == PACEQ_CAP);
    CHECK(pace_epoll_timeout_ms(&g_p, t_end) == -1);
}

static void test_timeout(void)
{
    const uint64_t t0 = 1 * SEC;
    pace_reset(&g_p, PACE_ANCHOR_FIRST, 1.0);
    CHECK(pace_epoll_timeout_ms(&g_p, t0) == -1); /* idle */

    push(1, 0, 701u, t0);  /* base anchor: target t0 */
    push(2, 10, 702u, t0); /* target t0 + 10 ms */
    linsight_can_record_t out[4];
    CHECK(pace_pop_due(&g_p, t0, out, 4) == 1); /* drop the anchor */
    CHECK(out[0].id == 701u);
    CHECK(pace_epoll_timeout_ms(&g_p, t0) == 10);
    CHECK(pace_epoll_timeout_ms(&g_p, t0 + 5 * MS) == 5);
    CHECK(pace_epoll_timeout_ms(&g_p, t0 + 4 * MS + 1) == 6); /* ceil 5.999999 */
    CHECK(pace_epoll_timeout_ms(&g_p, t0 + 10 * MS - 1) == 1); /* 1 ns left -> 1 ms */
    CHECK(pace_epoll_timeout_ms(&g_p, t0 + 10 * MS) == 0);     /* due */
    CHECK(pace_epoll_timeout_ms(&g_p, t0 + 11 * MS) == 0);     /* backlog */

    /* Far-future targets clamp at INT_MAX. */
    pace_reset(&g_p, PACE_ANCHOR_FIRST, 1.0);
    push(1, 0, 703u, t0);
    push(2, 3000000000u, 704u, t0); /* +3e9 ms > INT_MAX ms */
    CHECK(pace_pop_due(&g_p, t0, out, 4) == 1);
    CHECK(pace_epoll_timeout_ms(&g_p, t0) == 0x7FFFFFFF);
}

static void test_pop_cap(void)
{
    const uint64_t t0 = 1 * SEC;
    pace_reset(&g_p, PACE_ANCHOR_FIRST, 1.0);
    for (uint32_t i = 0; i < 5; i++) {
        push((uint16_t)i, 0, 800u + i, t0); /* one shared target */
    }
    CHECK(g_p.merged == 4);
    linsight_can_record_t out[8];
    CHECK(pace_pop_due(&g_p, t0, out, 3) == 3); /* cap truncates */
    CHECK(out[0].id == 800u && out[2].id == 802u);
    CHECK(pace_count(&g_p) == 2);
    CHECK(pace_pop_due(&g_p, t0, out, 0) == 0);
    CHECK(pace_count(&g_p) == 2);
    CHECK(pace_pop_due(&g_p, t0, out, 8) == 2);
    CHECK(out[0].id == 803u && out[1].id == 804u);
    CHECK(pace_count(&g_p) == 0);
}

static void test_wraparound(void)
{
    /* u16 seq wraps 0xFFFF -> 0: records survive intact and in order. */
    const uint64_t t0 = 1 * SEC;
    pace_reset(&g_p, PACE_ANCHOR_FIRST, 1.0);
    push(0xFFFE, 0, 901u, t0);
    push(0xFFFF, 10, 902u, t0);
    push(0x0000, 20, 903u, t0);
    push(0x0001, 30, 904u, t0);
    linsight_can_record_t out[8];
    CHECK(pace_pop_due(&g_p, t0 + 30 * MS, out, 8) == 4);
    CHECK(out[0].seq == 0xFFFE && out[1].seq == 0xFFFF);
    CHECK(out[2].seq == 0x0000 && out[3].seq == 0x0001);
    CHECK(out[2].id == 903u);

    /* ts_ms wrap 0xFFFFFFF0 -> 0x5 counts as backwards: delta 0. */
    pace_reset(&g_p, PACE_ANCHOR_FIRST, 1.0);
    push(1, 0xFFFFFFF0u, 905u, t0);
    push(2, 0x00000005u, 906u, t0);
    CHECK(g_p.merged == 1);
    CHECK(pace_pop_due(&g_p, t0 - 1, out, 8) == 0);
    CHECK(pace_pop_due(&g_p, t0, out, 8) == 2);
    CHECK(out[0].id == 905u && out[1].id == 906u);
}

int main(void)
{
    test_wall_anchor();
    test_wall_late();
    test_first_base_speed();
    test_new_session();
    test_ts_backwards();
    test_same_ms_batch();
    test_full_drop_oldest();
    test_timeout();
    test_pop_cap();
    test_wraparound();
    puts("pace: ok");
    return 0;
}
