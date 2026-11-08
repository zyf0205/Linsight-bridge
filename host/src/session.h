#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t proto;
    char fw[32];
    char device_id[16];
    uint32_t boot_count;
    uint32_t uptime_ms;
    uint32_t bitrate;
    bool has_device_id;
    bool has_boot;
    bool has_uptime;
} session_hello_t;

/* Per-device dedupe/gap state. `last_ts_ms` is the primary monotonic key;
 * the wrapped u16 seq is only used within one millisecond. */
typedef struct {
    uint32_t boot_count;
    bool have_boot;
    bool have_mark;
    uint32_t last_ts_ms;
    uint16_t last_seq;
    uint64_t accepted;
    uint64_t skipped;
    uint64_t gaps;
    uint64_t missing;
    uint64_t ambiguous;
} session_state_t;

/* Parse the additive HELLO fields. Returns false only for a malformed or
 * oversized payload; missing keys remain zero/false for old firmware. */
bool session_parse_hello(const uint8_t *payload, uint16_t len, session_hello_t *out);

/* Wall time corresponding to device uptime 0. */
uint64_t session_anchor_start_wall_ms(uint64_t host_wall_ms, uint32_t uptime_ms);

/* Reset dedupe marks when the device rebooted (boot_count changed). */
void session_reset(session_state_t *s, uint32_t boot_count, bool have_boot);

/* Zero the per-session statistics while keeping the dedupe marks, so a
 * reconnect can still skip its backlog. */
void session_clear_counters(session_state_t *s);

/* Returns true when the record should be accepted. A reconnect backlog
 * (ts older than the mark) is skipped; gaps are counted, not invented. */
bool session_record_new(session_state_t *s, const linsight_can_record_t *rec);

#ifdef __cplusplus
}
#endif
