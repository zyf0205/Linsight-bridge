#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "frame.h"

void pacer_init(bool enabled);
bool pacer_enabled(void);

/* Anchor device milliseconds onto CLOCK_MONOTONIC nanoseconds. */
void pacer_anchor(uint64_t now_ns, uint32_t dev_ms);

/* Queue records at their 1:1 target times, or forward immediately when pacing
 * is disabled. */
void pacer_submit(const linsight_can_record_t *recs, uint32_t n, uint64_t now_ns);

/* Forward records whose target time has passed. */
void pacer_pump(void);

/* Flush the whole queue regardless of target time (shutdown). */
void pacer_drain(void);

/* epoll timeout until the next due record, or -1 when the queue is empty. */
int pacer_epoll_timeout_ms(uint64_t now_ns);

uint32_t pacer_pending(void);
