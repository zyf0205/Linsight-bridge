#pragma once

/*
 * Built-in CAN traffic simulator (bench / demo mode).
 *
 * Generates synthetic frames straight into the daemon ingest path - same
 * route as real device batches - so the whole stack (writer, SSE, CLI live)
 * is exercised without any CAN hardware. Two sources:
 *
 *   - patterns: named frame sets (engine, bms, mix, counter) generated on a
 *     10 Hz timer at an aggregate rate of 1..1000 fps
 *   - inject: one-shot frames pushed from the CLI / web dashboard
 *
 * The simulator never talks to the device; it is host-side only.
 */

#include <stdbool.h>
#include <stdint.h>

#include "frame.h"

typedef enum {
    SIM_PATTERN_NONE = 0,
    SIM_PATTERN_ENGINE,  /* ECU: RPM / coolant / throttle on 0x0C0 */
    SIM_PATTERN_BMS,     /* battery pack: 0x122 pack V/I/SOC + 0x123 cells */
    SIM_PATTERN_MIX,     /* engine + BMS + wheel speeds + body states */
    SIM_PATTERN_COUNTER, /* can_soak.py-compatible 32-bit counter on 0x123 */
} sim_pattern_t;

typedef struct {
    bool running;
    sim_pattern_t pattern;
    unsigned rate_fps;   /* aggregate frames per second */
    uint64_t generated;  /* total frames produced since daemon start */
    char last_error[64]; /* why the last command failed, "" = ok */
} sim_status_t;

/* One generated batch lands here. The callback reuses the real ingest path
 * (writer ring + live clients + SSE) so simulated traffic is indistinguishable
 * from device traffic downstream. */
typedef void (*sim_emit_cb)(void *ctx, const linsight_can_record_t *recs, uint32_t n);

/* Start generating. rate is clamped to 1..1000 fps. Returns false when the
 * pattern name is unknown or already running. */
bool sim_start(const char *pattern, unsigned rate_fps, sim_emit_cb emit, void *ctx);
/* Stop generating. Idempotent. */
void sim_stop(void);

/* Push a single frame immediately (dlc 0..8, data may be NULL when dlc==0).
 * Works whether or not a pattern is running. Returns false on bad arguments. */
bool sim_inject(uint32_t id, bool ext, bool rtr, uint8_t dlc, const uint8_t *data,
                sim_emit_cb emit, void *ctx);

/* Called from the daemon's 10 Hz timer tick; emits whatever the current rate
 * requires. Safe to call when not running (no-op). */
void sim_tick(sim_emit_cb emit, void *ctx);

/* Daemon-side uptime in ms (same axis the device uses for ts_ms). */
uint32_t sim_now_ms(void);

void sim_status(sim_status_t *out);
/* Parse "engine|bms|mix|counter" -> enum; false when unknown. */
bool sim_pattern_parse(const char *name, sim_pattern_t *out);
const char *sim_pattern_name(sim_pattern_t p);
