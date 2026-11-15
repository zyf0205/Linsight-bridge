#pragma once

/*
 * pace.h — non-blocking pacing queue for the LinSight host bridge.
 *
 * Maps device timestamps (ts_ms) to absolute CLOCK_MONOTONIC target moments
 * and holds the records in a drop-oldest ring. The epoll main loop only
 * compares targets against now_ns — a "sleep_until" variant that never
 * blocks: a record whose target has already passed pops immediately
 * (reconnect backlog is compressed, live traffic stays real-time) and the
 * queue never plays catch-up with sleeps.
 *
 * All state is plain data and now_ns is passed in by the caller, so the
 * module has no POSIX clock dependency and is deterministic.
 *
 * NOTE: pace_t embeds PACEQ_CAP entries (~8 MiB). Allocate it statically or
 * on the heap, never on the stack.
 */

#include <stdbool.h>
#include <stdint.h>

#include "frame.h"

#define PACEQ_CAP 262144u       /* drop-oldest + counters, like can_ring */
#define PACE_MIN_DELTA_US 1000u /* target gaps < 1 ms snap onto the previous
                                 * target (same-ms timestamps batch together) */

typedef struct {
    linsight_can_record_t rec;
    uint64_t target_ns;
} pace_entry_t;

typedef enum {
    PACE_ANCHOR_WALL = 0,  /* target = (anchor_wall_ms + ts_ms) * 1e6 + wall_offset_ns */
    PACE_ANCHOR_FIRST = 1, /* target = base_ns + (ts_ms - base_dev_ms) * 1e6 / speed */
} pace_anchor_t;

typedef struct {
    pace_entry_t q[PACEQ_CAP]; /* static ring: head = oldest entry, count live */
    uint32_t head, count;
    pace_anchor_t mode;
    double speed;            /* FIRST-mode replay rate (2.0 = twice as fast) */
    bool have_last;          /* a target has been assigned (survives session seams) */
    uint32_t last_dev_ms;
    uint64_t last_target_ns;
    bool have_base;          /* FIRST-mode base established */
    uint64_t base_ns;
    uint32_t base_dev_ms;
    bool wall_bound;
    uint64_t wall_offset_ns; /* mono_ns - wall_ms*1e6, sampled at bind time */
    uint64_t anchor_wall_ms;
    uint64_t dropped, merged; /* overflow drops (oldest) / same-target batches */
} pace_t;

/* Fresh state for the given anchor mode. speed <= 0 is treated as 1.0. */
void pace_reset(pace_t *p, pace_anchor_t mode, double speed);

/* WALL mode: bind the wall clock to CLOCK_MONOTONIC. anchor_wall_ms is the
 * wall-clock ms matching device ts_ms = 0; now_ns / now_wall_ms are
 * simultaneous monotonic / wall samples. Rebinding restarts the ts_ms domain
 * (a reconnecting device counts near 0 again) while queued targets keep
 * their order. */
void pace_bind_wall(pace_t *p, uint64_t anchor_wall_ms, uint64_t now_ns,
                    uint64_t now_wall_ms);

/* Session seam (device reconnect / replay session change). The queue is NOT
 * cleared; in FIRST mode the next record re-bases at
 * max(now_ns, last_target_ns + 1 ms) so fresh (wrapped) timestamps cannot
 * jump ahead of the queued tail. WALL mode re-anchors via pace_bind_wall(). */
void pace_new_session(pace_t *p, uint64_t now_ns);

/* Compute the target moment and enqueue (drop-oldest when full). now_ns is
 * used for the FIRST-mode base and the WALL fallback before binding. */
void pace_push(pace_t *p, const linsight_can_record_t *r, uint64_t now_ns);

/* Pop every entry with target <= now_ns, oldest first, at most cap into out.
 * Returns the number popped; the rest stay queued. cap == 0 pops nothing. */
uint32_t pace_pop_due(pace_t *p, uint64_t now_ns, linsight_can_record_t *out,
                      uint32_t cap);

/* epoll timeout hint: -1 = idle (queue empty), 0 = head due now (backlog
 * pops immediately), else ceil ms until the head target (>= 1, capped). */
int pace_epoll_timeout_ms(const pace_t *p, uint64_t now_ns);

uint32_t pace_count(const pace_t *p);
