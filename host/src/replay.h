#pragma once

#include <stdint.h>

typedef struct {
    uint64_t frames;
    uint64_t elapsed_ns;
    uint64_t late_max_ns;
    uint64_t late_p99_ns;
} replay_stats_t;

/*
 * Replay an .lsc file on a SocketCAN interface at the recorded inter-frame
 * spacing. speed scales time (2.0 = twice as fast). Timestamps are scheduled
 * against CLOCK_MONOTONIC with TIMER_ABSTIME so per-frame sleep drift does not
 * accumulate.
 *
 * Returns 0 on success, -1 on error (errno is set).
 */
int canlog_replay(const char *lsc_path, const char *iface, double speed,
                  replay_stats_t *stats);
