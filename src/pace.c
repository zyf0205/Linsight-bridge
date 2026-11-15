#include "pace.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PACE_NS_PER_MS 1000000ULL
#define PACE_MERGE_NS ((uint64_t)PACE_MIN_DELTA_US * 1000ULL) /* 1 ms */
#define PACE_TIMEOUT_MAX_MS 0x7FFFFFFF                        /* INT_MAX */

static uint64_t pace_wall_target(const pace_t *p, const linsight_can_record_t *r)
{
    return (p->anchor_wall_ms + (uint64_t)r->ts_ms) * PACE_NS_PER_MS + p->wall_offset_ns;
}

void pace_reset(pace_t *p, pace_anchor_t mode, double speed)
{
    memset(p, 0, sizeof(*p));
    p->mode = mode;
    p->speed = speed;
}

void pace_bind_wall(pace_t *p, uint64_t anchor_wall_ms, uint64_t now_ns,
                    uint64_t now_wall_ms)
{
    p->anchor_wall_ms = anchor_wall_ms;
    p->wall_offset_ns = now_ns - now_wall_ms * PACE_NS_PER_MS;
    p->wall_bound = true;
    /* A fresh anchor starts a new ts_ms domain (the device counts near 0
     * again); have_last keeps queued targets monotonic across the seam. */
    p->last_dev_ms = 0;
}

void pace_new_session(pace_t *p, uint64_t now_ns)
{
    if (p->mode != PACE_ANCHOR_FIRST) {
        return; /* WALL re-anchors through pace_bind_wall() */
    }
    /* Stash base = max(now_ns, last_target_ns + 1 ms); the next push
     * re-bases on it so fresh (wrapped) timestamps cannot jump ahead of
     * the queued tail. */
    uint64_t base = now_ns;
    if (p->have_last && p->last_target_ns + PACE_NS_PER_MS > base) {
        base = p->last_target_ns + PACE_NS_PER_MS;
    }
    p->base_ns = base;
    p->have_base = false;
}

void pace_push(pace_t *p, const linsight_can_record_t *r, uint64_t now_ns)
{
    uint64_t target;
    if (p->mode == PACE_ANCHOR_FIRST && !p->have_base) {
        /* First record of a base: fresh reset starts at now, a session seam
         * starts at the stashed floor (already past -> pops immediately). */
        if (now_ns > p->base_ns) {
            p->base_ns = now_ns;
        }
        p->base_dev_ms = r->ts_ms;
        p->have_base = true;
        target = p->base_ns;
    } else if (p->have_last && r->ts_ms < p->last_dev_ms) {
        target = p->last_target_ns; /* ts moved backwards: delta 0, no reorder */
    } else if (p->mode == PACE_ANCHOR_WALL) {
        target = p->wall_bound ? pace_wall_target(p, r) : now_ns;
    } else {
        const double spd = p->speed > 0.0 ? p->speed : 1.0;
        const uint32_t delta_ms = r->ts_ms - p->base_dev_ms;
        target = p->base_ns + (uint64_t)((double)delta_ms * (double)PACE_NS_PER_MS / spd);
    }

    /* Batch onto the previous target when the gap is < 1 ms (covers equal
     * timestamps, backwards ts and sub-ms speed-scaled gaps). */
    if (p->have_last &&
        (target < p->last_target_ns || target - p->last_target_ns < PACE_MERGE_NS)) {
        target = p->last_target_ns;
        p->merged++;
    }

    if (p->count == PACEQ_CAP) { /* full: drop the oldest entry (can_ring) */
        p->head = (p->head + 1u) % PACEQ_CAP;
        p->count--;
        p->dropped++;
    }
    p->q[(p->head + p->count) % PACEQ_CAP] =
        (pace_entry_t){.rec = *r, .target_ns = target};
    p->count++;
    p->have_last = true;
    p->last_dev_ms = r->ts_ms;
    p->last_target_ns = target;
}

uint32_t pace_pop_due(pace_t *p, uint64_t now_ns, linsight_can_record_t *out,
                      uint32_t cap)
{
    uint32_t n = 0;
    while (n < cap && p->count > 0 && p->q[p->head].target_ns <= now_ns) {
        if (out) {
            out[n] = p->q[p->head].rec;
        }
        n++;
        p->head = (p->head + 1u) % PACEQ_CAP;
        p->count--;
    }
    return n;
}

int pace_epoll_timeout_ms(const pace_t *p, uint64_t now_ns)
{
    if (p->count == 0) {
        return -1;
    }
    const uint64_t target = p->q[p->head].target_ns;
    if (target <= now_ns) {
        return 0;
    }
    const uint64_t diff = target - now_ns;
    uint64_t ms = diff / PACE_NS_PER_MS + (diff % PACE_NS_PER_MS != 0 ? 1u : 0u);
    if (ms > (uint64_t)PACE_TIMEOUT_MAX_MS) {
        ms = (uint64_t)PACE_TIMEOUT_MAX_MS;
    }
    return (int)ms;
}

uint32_t pace_count(const pace_t *p)
{
    return p->count;
}
