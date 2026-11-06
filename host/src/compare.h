#pragma once

#include <stdint.h>

/*
 * Offline comparison of two .lsc recordings.
 *
 * Frames are matched FIFO per (id, flags, dlc, data) key: the n-th occurrence
 * of a key in the reference must pair with the n-th occurrence in the
 * candidate. Payload bytes beyond dlc are ignored.
 *
 * "extra" counts candidate frames left over after matching, which includes
 * duplicates. "order" counts matched frames whose candidate position is
 * earlier than the previous match (same set of frames, different order).
 */
typedef struct {
    uint64_t ref_frames;
    uint64_t cand_frames;
    uint64_t matched;
    uint64_t missing; /* in reference, not in candidate */
    uint64_t extra;   /* in candidate, not in reference (incl. duplicates) */
    uint64_t order;   /* matched out of order */
} compare_stats_t;

/* Returns 0 on success, -1 with errno set on file/allocation errors. */
int canlog_compare(const char *ref_path, const char *cand_path, compare_stats_t *stats);
