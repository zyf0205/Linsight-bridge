#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "frame.h"

/*
 * .lsc binary log: 32-byte header + raw 20-byte records.
 * Header: magic "LSC1" | u16 version | u32 bitrate | u32 flags |
 *         u64 unix-seconds at session start | reserved to 32 bytes.
 * A truncated tail record is tolerated when reading (crash recovery).
 */

#define LSC_HEADER_SIZE 32u
#define LSC_VERSION 1u

typedef struct {
    FILE *f;
    uint64_t written;
    uint32_t bitrate;
    uint64_t start_wall; /* unix seconds at session start */
} lsc_writer_t;

int lsc_writer_open(lsc_writer_t *w, const char *path, uint32_t bitrate, uint64_t start_wall);
/* Returns 0 on success, -1 on write error. */
int lsc_writer_write(lsc_writer_t *w, const linsight_can_record_t *r);
int lsc_writer_flush(lsc_writer_t *w);
void lsc_writer_close(lsc_writer_t *w);

/* Returns the number of records passed to cb, or -1 on open error. */
typedef void (*lsc_record_cb)(const linsight_can_record_t *r, void *ctx);
long lsc_read(const char *path, lsc_record_cb cb, void *ctx);

/* Sequential reader used by replay/compare. lsc_reader_open validates the
 * magic and version. lsc_reader_next returns 1 on a record, 0 at EOF, -1 on a
 * read error (a truncated tail record is treated as EOF). */
typedef struct {
    FILE *f;
    uint32_t bitrate;
    uint64_t start_wall;
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

/* Exports. Both return the number of frames written, or -1. */
long canlog_export_asc(const char *lsc_path, const char *out_path);
long canlog_export_candump(const char *lsc_path, const char *out_path);
long canlog_export_asc_filtered(const char *lsc_path, const char *out_path,
                                const lsc_filter_t *filter);
long canlog_export_candump_filtered(const char *lsc_path, const char *out_path,
                                    const lsc_filter_t *filter);

/* Format one record as a candump line using the given session start time.
 * Returns the string length, or -1 when buf is too small. */
int canlog_format_candump(const linsight_can_record_t *r, uint64_t start_wall, char *buf,
                          size_t cap);
