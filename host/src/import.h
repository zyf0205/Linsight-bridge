#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t records;
    uint32_t boot_count;
    uint32_t bitrate;
    uint32_t flags;
    uint64_t start_wall_ms;
    uint64_t gaps;
    uint64_t missing;
    uint64_t backwards;
    uint64_t duplicates;
    uint64_t ambiguous;
    bool truncated;
    bool uptime_only;
} lsc_import_info_t;

/*
 * Copy an .lsc file, re-anchoring device uptime-only logs:
 *   - wall-valid input: preserve its anchor;
 *   - --boot-wall: use the supplied Unix epoch (seconds);
 *   - otherwise: anchor from the file mtime minus the last record's ts_ms.
 * Returns 0 on success, -1 on error (err filled in).
 */
int lsc_import(const char *src, const char *dst, bool have_boot_wall,
               uint64_t boot_wall_sec, lsc_import_info_t *out, char *err, size_t errcap);

/*
 * Validate header, record length and sequence continuity. Returns 0 when the
 * file is acceptable, 1 when --strict finds gaps/duplicates/backwards time,
 * and 2 for a corrupt header or read error.
 */
int lsc_verify(const char *path, bool strict, lsc_import_info_t *out, char *err,
               size_t errcap);

/* Print any .met / .trg sidecars that sit next to `lsc_path`. */
void lsc_print_sidecars(const char *lsc_path, FILE *out);

#ifdef __cplusplus
}
#endif
