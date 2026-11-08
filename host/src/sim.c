#include "sim.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define SIM_TICK_HZ 10u
#define SIM_RATE_MIN 1u
#define SIM_RATE_MAX 1000u
#define SIM_BURST_MAX 100u /* frames per tick cap = 1000 fps at 10 Hz */

static struct {
    bool running;
    sim_pattern_t pattern;
    unsigned rate_fps;
    uint64_t generated;
    uint32_t tick;  /* 10 Hz ticks since start; drives signal evolution */
    double carry;   /* fractional frames owed from previous ticks */
    uint16_t seq;   /* record sequence, same semantics as the device */
    char error[64];
} s_sim;

uint32_t sim_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

/* Triangle wave 0..amp over `period` counts: a period signal without libm. */
static uint32_t tri(uint32_t phase, uint32_t period, uint32_t amp)
{
    if (period < 2u) {
        return 0;
    }
    const uint32_t half = period / 2u;
    const uint32_t x = phase % period;
    const uint32_t v = x < half ? x : period - x;
    return amp * v / half;
}

static uint32_t put_u16_le(uint8_t *d, uint16_t v)
{
    d[0] = (uint8_t)(v & 0xffu);
    d[1] = (uint8_t)(v >> 8);
    return 2u;
}

static uint32_t put_u32_le(uint8_t *d, uint32_t v)
{
    put_u16_le(d, (uint16_t)(v & 0xffffu));
    put_u16_le(d + 2, (uint16_t)(v >> 16));
    return 4u;
}

/* Build one record of the current pattern. seq is a per-frame monotone u16
 * exactly like the device's: `verify` and reconnect dedupe both key on it, so
 * simulated traffic must carry a real sequence or it looks like duplicates. */
static uint32_t pattern_record(linsight_can_record_t *r)
{
    memset(r, 0, sizeof(*r));
    r->ts_ms = sim_now_ms();
    r->seq = s_sim.seq;

    switch (s_sim.pattern) {
    case SIM_PATTERN_ENGINE: {
        /* 0x0C0: RPM(0.25 rpm/bit) | coolant C | throttle % */
        const uint32_t rpm = 800u + tri(s_sim.tick, 100u, 3200u);
        r->id = 0x0C0u;
        r->dlc = 6;
        uint32_t off = put_u16_le(r->data, (uint16_t)(rpm / 4u));
        off += put_u16_le(r->data + off, (uint16_t)(70u + tri(s_sim.tick, 130u, 60u)));
        off += put_u16_le(r->data + off, (uint16_t)(10u + tri(s_sim.tick, 37u, 80u)));
        (void)off;
        return 1u;
    }
    case SIM_PATTERN_BMS: {
        if (s_sim.tick % 2u == 0u) {
            /* 0x122 pack: volts/100 mV | signed-ish current mA | SOC% */
            r->id = 0x122u;
            r->dlc = 5;
            uint32_t off = put_u16_le(r->data, 720u);
            off += put_u16_le(r->data + off, (uint16_t)(32768u - 16384u +
                                                        tri(s_sim.tick, 50u, 20000u)));
            r->data[off] = (uint8_t)(20u + tri(s_sim.tick, 200u, 75u));
            return 1u;
        }
        /* 0x123 cell extrema mV x4 */
        r->id = 0x123u;
        r->dlc = 8;
        for (int i = 0; i < 4; i++) {
            put_u16_le(r->data + (size_t)i * 2u, (uint16_t)(3650u + (uint32_t)i * 7u));
        }
        return 1u;
    }
    case SIM_PATTERN_MIX: {
        /* Rotate through 5 IDs; each frame lands on a different member. */
        static const uint32_t ids[] = {0x0C0u, 0x122u, 0x0D2u, 0x2A1u, 0x3E5u};
        const uint32_t which = s_sim.tick % 5u;
        r->id = ids[which];
        switch (which) {
        case 0u:
            r->dlc = 4;
            put_u16_le(r->data, (uint16_t)((1500u + tri(s_sim.tick, 80u, 2500u)) / 4u));
            put_u16_le(r->data + 2, (uint16_t)(75u + tri(s_sim.tick, 130u, 55u)));
            break;
        case 1u:
            r->dlc = 5;
            put_u16_le(r->data, 720u);
            put_u16_le(r->data + 2,
                       (uint16_t)(32768u - 16384u + tri(s_sim.tick, 50u, 20000u)));
            r->data[4] = (uint8_t)(55u + tri(s_sim.tick, 200u, 40u));
            break;
        case 2u: /* wheel speeds: km/h / 0.01, one per corner */
            r->dlc = 8;
            for (int w = 0; w < 4; w++) {
                put_u16_le(r->data + (size_t)w * 2u,
                           (uint16_t)(12000u + tri(s_sim.tick + (uint32_t)w, 60u, 4000u)));
            }
            break;
        case 3u: /* body: doors/lights bitmap */
            r->dlc = 2;
            put_u16_le(r->data, (uint16_t)(s_sim.tick % 16u));
            break;
        default: /* diagnostic: running counter + magic */
            r->dlc = 8;
            put_u32_le(r->data, (uint32_t)s_sim.generated);
            put_u16_le(r->data + 4, 0xBEEFu);
            break;
        }
        return 1u;
    }
    case SIM_PATTERN_COUNTER: {
        /* can_soak.py layout: LE u32 counter + 11 22 33 44 on 0x123, so the
         * soak verifier can check the sequence end-to-end. */
        r->id = 0x123u;
        r->dlc = 8;
        put_u32_le(r->data, (uint32_t)s_sim.generated);
        memcpy(r->data + 4, "\x11\x22\x33\x44", 4);
        return 1u;
    }
    default:
        return 0u;
    }
}

bool sim_pattern_parse(const char *name, sim_pattern_t *out)
{
    if (!name || !out) {
        return false;
    }
    if (strcmp(name, "engine") == 0) {
        *out = SIM_PATTERN_ENGINE;
    } else if (strcmp(name, "bms") == 0) {
        *out = SIM_PATTERN_BMS;
    } else if (strcmp(name, "mix") == 0) {
        *out = SIM_PATTERN_MIX;
    } else if (strcmp(name, "counter") == 0) {
        *out = SIM_PATTERN_COUNTER;
    } else {
        return false;
    }
    return true;
}

const char *sim_pattern_name(sim_pattern_t p)
{
    switch (p) {
    case SIM_PATTERN_ENGINE:
        return "engine";
    case SIM_PATTERN_BMS:
        return "bms";
    case SIM_PATTERN_MIX:
        return "mix";
    case SIM_PATTERN_COUNTER:
        return "counter";
    default:
        return "none";
    }
}

bool sim_start(const char *pattern, unsigned rate_fps, sim_emit_cb emit, void *ctx)
{
    sim_pattern_t p;
    if (!sim_pattern_parse(pattern, &p)) {
        snprintf(s_sim.error, sizeof(s_sim.error), "unknown pattern");
        return false;
    }
    if (s_sim.running) {
        snprintf(s_sim.error, sizeof(s_sim.error), "already running");
        return false;
    }
    if (!emit) {
        snprintf(s_sim.error, sizeof(s_sim.error), "no emit callback");
        return false;
    }
    if (rate_fps < SIM_RATE_MIN) {
        rate_fps = SIM_RATE_MIN;
    }
    if (rate_fps > SIM_RATE_MAX) {
        rate_fps = SIM_RATE_MAX;
    }
    s_sim.pattern = p;
    s_sim.rate_fps = rate_fps;
    s_sim.running = true;
    s_sim.tick = 0;
    s_sim.carry = 0.0;
    s_sim.error[0] = '\0';
    /* One frame now so the UI shows life without waiting a full tick. */
    sim_tick(emit, ctx);
    return true;
}

void sim_stop(void)
{
    if (s_sim.running) {
        s_sim.running = false;
        s_sim.error[0] = '\0';
    }
}

bool sim_inject(uint32_t id, bool ext, bool rtr, uint8_t dlc, const uint8_t *data,
                sim_emit_cb emit, void *ctx)
{
    /* RTR frames carry a DLC but no data field, so data may be NULL. */
    if (!emit || dlc > LINSIGHT_DLC_MAX || (dlc > 0 && !rtr && !data)) {
        return false;
    }
    if (ext && id > 0x1FFFFFFFu) {
        return false;
    }
    if (!ext && id > 0x7FFu) {
        return false;
    }
    linsight_can_record_t r;
    memset(&r, 0, sizeof(r));
    r.id = id;
    r.ts_ms = sim_now_ms();
    r.seq = s_sim.seq++;
    r.flags = (uint8_t)((ext ? LINSIGHT_REC_FLAG_EXT : 0u) |
                        (rtr ? LINSIGHT_REC_FLAG_RTR : 0u));
    r.dlc = dlc;
    if (dlc && data) {
        memcpy(r.data, data, dlc); /* RTR: data stays zeroed */
    }
    s_sim.generated++;
    emit(ctx, &r, 1);
    return true;
}

void sim_tick(sim_emit_cb emit, void *ctx)
{
    if (!s_sim.running || !emit) {
        return;
    }
    /* frames-per-tick split into integer part + fractional carry so slow
     * rates (e.g. 3 fps) average out over ticks instead of quantizing. */
    const double want = (double)s_sim.rate_fps / (double)SIM_TICK_HZ + s_sim.carry;
    uint32_t n = (uint32_t)want;
    s_sim.carry = want - (double)n;
    if (n > SIM_BURST_MAX) {
        n = SIM_BURST_MAX;
    }

    linsight_can_record_t recs[SIM_BURST_MAX];
    uint32_t made = 0;
    for (uint32_t i = 0; i < n; i++) {
        made += pattern_record(&recs[made]);
        s_sim.seq++; /* u16 wrap is the device's own sequence semantics */
        s_sim.tick++;
    }
    if (made) {
        s_sim.generated += made;
        emit(ctx, recs, made);
    }
}

/* Test hook: the counter pattern embeds this value; tests use it instead of
 * hard-coding counts that depend on what ran before. */
uint32_t sim_snapshot_generated(void)
{
    return (uint32_t)s_sim.generated;
}

uint16_t sim_snapshot_seq(void)
{
    return s_sim.seq;
}

void sim_status(sim_status_t *out)
{
    if (!out) {
        return;
    }
    out->running = s_sim.running;
    out->pattern = s_sim.pattern;
    out->rate_fps = s_sim.rate_fps;
    out->generated = s_sim.generated;
    memcpy(out->last_error, s_sim.error, sizeof(out->last_error));
}
