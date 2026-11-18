#define _DEFAULT_SOURCE

#include "pacer.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "util.h"
#include "vcan.h"

#define PACEQ_CAP 262144u              /* 262k-entry drop-oldest ring (~8 MiB) */
#define PACE_NS_PER_MS 1000000ULL      /* 1 ms in nanoseconds */
#define PACE_PUMP_MAX 4096u            /* max frames per epoll iteration */
#define PACE_PUMP_BUDGET_NS 5000000ULL /* max 5 ms pump time per iteration */

typedef struct {
    linsight_can_record_t rec;
    uint64_t target_ns;
} pace_entry_t;

typedef struct {
    pace_entry_t q[PACEQ_CAP];
    uint32_t head;
    uint32_t count;
    bool anchored;
    uint64_t anchor_mono_ns;
    uint32_t anchor_dev_ms;
    bool have_last;
    uint32_t last_dev_ms;
    uint64_t last_target_ns;
} pace_t;

static pace_t g_pace;
static bool g_enabled = true;

static void pace_anchor(pace_t *p, uint64_t now_ns, uint32_t dev_ms)
{
    p->anchor_mono_ns = now_ns;
    p->anchor_dev_ms = dev_ms;
    p->anchored = true;
    p->last_dev_ms = 0;
}

static void pace_push(pace_t *p, const linsight_can_record_t *r, uint64_t now_ns)
{
    uint64_t target = now_ns;
    if (p->have_last && r->ts_ms < p->last_dev_ms) {
        target = p->last_target_ns;
    } else if (p->anchored && r->ts_ms > p->anchor_dev_ms) {
        target = p->anchor_mono_ns +
                 (uint64_t)(r->ts_ms - p->anchor_dev_ms) * PACE_NS_PER_MS;
    } else if (p->anchored) {
        target = p->anchor_mono_ns;
    }

    if (p->have_last &&
        (target < p->last_target_ns || target - p->last_target_ns < PACE_NS_PER_MS)) {
        target = p->last_target_ns;
    }

    if (p->count == PACEQ_CAP) {
        p->head = (p->head + 1u) % PACEQ_CAP;
        p->count--;
    }
    p->q[(p->head + p->count) % PACEQ_CAP] =
        (pace_entry_t){.rec = *r, .target_ns = target};
    p->count++;
    p->have_last = true;
    p->last_dev_ms = r->ts_ms;
    p->last_target_ns = target;
}

static uint32_t pace_pop_due(pace_t *p, uint64_t now_ns, linsight_can_record_t *out,
                             uint32_t cap)
{
    uint32_t n = 0;
    while (n < cap && p->count > 0 && p->q[p->head].target_ns <= now_ns) {
        out[n++] = p->q[p->head].rec;
        p->head = (p->head + 1u) % PACEQ_CAP;
        p->count--;
    }
    return n;
}

void pacer_init(bool enabled)
{
    memset(&g_pace, 0, sizeof(g_pace));
    g_enabled = enabled;
}

bool pacer_enabled(void)
{
    return g_enabled;
}

void pacer_anchor(uint64_t now_ns, uint32_t dev_ms)
{
    pace_anchor(&g_pace, now_ns, dev_ms);
}

void pacer_submit(const linsight_can_record_t *recs, uint32_t n, uint64_t now_ns)
{
    if (!g_enabled) {
        vcan_forward(recs, n);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        pace_push(&g_pace, &recs[i], now_ns);
    }
}

void pacer_pump(void)
{
    if (!g_enabled || !vcan_is_open()) {
        return;
    }
    static linsight_can_record_t out[256];
    const uint64_t t0 = mono_ns();
    uint32_t budget = PACE_PUMP_MAX;
    while (budget) {
        const uint32_t want = budget < 256u ? budget : 256u;
        const uint32_t got = pace_pop_due(&g_pace, mono_ns(), out, want);
        if (!got) {
            break;
        }
        vcan_forward(out, got);
        budget -= got;
        if (mono_ns() - t0 > PACE_PUMP_BUDGET_NS) {
            break;
        }
    }
}

void pacer_drain(void)
{
    if (!g_enabled) {
        return;
    }
    static linsight_can_record_t out[256];
    uint32_t got;
    while ((got = pace_pop_due(&g_pace, UINT64_MAX, out, 256u)) > 0) {
        vcan_forward(out, got);
    }
}

int pacer_epoll_timeout_ms(uint64_t now_ns)
{
    if (g_pace.count == 0) {
        return -1;
    }
    const uint64_t target = g_pace.q[g_pace.head].target_ns;
    if (target <= now_ns) {
        return 0;
    }
    const uint64_t diff = target - now_ns;
    const uint64_t ms = diff / PACE_NS_PER_MS + (diff % PACE_NS_PER_MS != 0 ? 1u : 0u);
    return ms > 0x7FFFFFFFull ? 0x7FFFFFFF : (int)ms;
}

uint32_t pacer_pending(void)
{
    return g_pace.count;
}
