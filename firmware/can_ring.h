#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PSRAM ring buffer of CAN records (one line of the plan's "twai_task -> PSRAM
 * ring" pipeline). Single producer (twai task), one consumer: net_task pops
 * records to send over TCP. The local trace does not read this ring; it uses
 * twai's capture-side UI history so streaming cannot blank the display.
 * All entry points take an internal mutex; hold times are memcpy-sized.
 *
 * Overflow policy is "drop old, keep new" (plan §8): pushing into a full ring
 * overwrites the oldest record and bumps the dropped counter, so the UI always
 * shows the latest bus activity and a reconnecting host receives the most
 * recent window. Sequence numbers make every gap visible downstream.
 */

typedef struct {
    uint32_t used;     /* records currently stored */
    uint32_t cap;      /* total capacity */
    uint32_t dropped;  /* records lost to overwrite */
    uint32_t stored;   /* records stored since boot (seq base) */
} can_ring_info_t;

/* Allocates capacity_records from PSRAM. Call once from app_main. */
esp_err_t can_ring_init(uint32_t capacity_records);

/* Takes ownership of the sequence number: rec->seq is assigned on success. */
void can_ring_push(linsight_can_record_t *rec);

/* Pop up to max records for streaming. Returns how many were copied. */
uint32_t can_ring_pop(linsight_can_record_t *out, uint32_t max);

void can_ring_get_info(can_ring_info_t *out);

/* Clear the overflow counter shown as OVF (Clear Stats); records are untouched. */
void can_ring_reset_dropped(void);

#ifdef __cplusplus
}
#endif
