#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "frame.h"

/*
 * .lsc binary log: 32-byte header + raw 20-byte records.
 * Header: magic "LSC1" | u16 version | u32 bitrate | u32 flags |
 *         u16 reserved | u64 unix-seconds at uptime 0 | u32 boot_count |
 *         u32 anchor_ms (millisecond remainder).
 * The reserved bytes keep old readers working (they only use start_wall).
 * A truncated tail record is tolerated when reading (crash recovery).
 */

#define LSC_HEADER_SIZE 32u
#define LSC_VERSION 1u

/* Header flags. Old readers ignore them. */
#define LSC_FLAG_TS_UPTIME_ONLY 0x1u /* start_wall is not a wall clock */
#define LSC_FLAG_ANCHOR_MS_VALID 0x2u /* anchor_ms is meaningful */
#define LSC_FLAG_BOOT_VALID 0x4u      /* boot_count is meaningful */

typedef struct {
    uint64_t start_wall_ms; /* wall time corresponding to device uptime 0 */
    uint32_t flags;         /* LSC_FLAG_* */
    uint32_t boot_count;
} lsc_open_info_t;

typedef struct {
    FILE *f;
    uint64_t written;
} lsc_writer_t;

int lsc_writer_open(lsc_writer_t *w, const char *path, uint32_t bitrate, uint64_t start_wall);
int lsc_writer_open_ex(lsc_writer_t *w, const char *path, uint32_t bitrate,
                       const lsc_open_info_t *info);
/* Returns 0 on success, -1 on write error. */
int lsc_writer_write(lsc_writer_t *w, const linsight_can_record_t *r);
int lsc_writer_flush(lsc_writer_t *w);
/* fflush + fdatasync; call at most once per second on slow storage. */
int lsc_writer_sync(lsc_writer_t *w);
/* Close always clears f; returns -1 on an existing stream or flush/close error. */
int lsc_writer_close(lsc_writer_t *w);

/* Sequential reader used by replay/compare. lsc_reader_open validates the
 * magic and version. lsc_reader_next returns 1 on a record, 0 at EOF, -1 on a
 * read error (a truncated tail record is treated as EOF). */
typedef struct {
    FILE *f;
    uint32_t bitrate;
    uint32_t flags;
    uint32_t boot_count;
    uint32_t anchor_ms;
    uint64_t start_wall;    /* compat seconds */
    uint64_t start_wall_ms; /* start_wall*1000 + anchor_ms */
    bool wall_valid;
    uint64_t remaining; /* snapshot of records present at open */
} lsc_reader_t;

int lsc_reader_open(lsc_reader_t *r, const char *path);
int lsc_reader_next(lsc_reader_t *r, linsight_can_record_t *rec);
void lsc_reader_close(lsc_reader_t *r);

/* Export filter. ext: -1 = any, 0 = standard only, 1 = extended only.
 * from/to are wall-clock seconds (inclusive from, exclusive to). */
typedef struct {
    bool id_valid;
    uint32_t id;
    int ext;
    bool from_valid;
    uint64_t from_wall;
    bool to_valid;
    uint64_t to_wall;
} lsc_filter_t;

/* Exports. All return the number of frames written, or -1. */
long canlog_export_asc_filtered(const char *lsc_path, const char *out_path,
                                const lsc_filter_t *filter);
long canlog_export_candump_filtered(const char *lsc_path, const char *out_path,
                                    const lsc_filter_t *filter);
long canlog_export_csv_filtered(const char *lsc_path, const char *out_path,
                                const lsc_filter_t *filter);

/* Format one record as a candump line using the given session start time.
 * Returns the string length, or -1 when buf is too small. */
int canlog_format_candump_ms(const linsight_can_record_t *r, uint64_t start_wall_ms, char *buf,
                             size_t cap);
