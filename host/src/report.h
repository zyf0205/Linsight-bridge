#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t frames;
    uint64_t unique_ids;
    uint64_t start_ts_ms;
    uint64_t end_ts_ms;
} lsc_report_info_t;

/*
 * Write a self-contained HTML report for an .lsc file.
 * out_path "-" writes to stdout. Returns 0 on success, -1 on error.
 */
int lsc_report_html(const char *lsc_path, const char *out_path, const char *title,
                    lsc_report_info_t *info);

#ifdef __cplusplus
}
#endif
