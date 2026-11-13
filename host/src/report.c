#include "report.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "canlog.h"
#include "frame.h"

#define REPORT_MAX_IDS 4096u
#define REPORT_TOP 12u
#define REPORT_POINTS 480u

typedef struct {
    uint32_t id;
    uint64_t count;
    uint8_t ext;
    uint8_t used;
} id_stat_t;

typedef struct {
    uint64_t frames;
    uint64_t std_frames;
    uint64_t ext_frames;
    uint64_t rtr_frames;
    uint64_t err_frames;
    uint64_t data_bytes;
    uint64_t total_bits;
    uint64_t first_ts;
    uint64_t last_ts;
    uint64_t unknown_ids;
    uint64_t start_wall_ms;
    uint32_t bitrate;
    uint32_t boot_count;
    uint32_t flags;
    bool wall_valid;
} report_ctx_t;

static uint32_t id_hash(uint32_t id, bool ext)
{
    return (id * 2654435761u + (ext ? 0x9E3779B9u : 0u)) & (REPORT_MAX_IDS - 1u);
}

static id_stat_t *id_get(id_stat_t *table, uint32_t id, bool ext)
{
    const uint32_t h = id_hash(id, ext);
    for (uint32_t i = 0; i < REPORT_MAX_IDS; i++) {
        id_stat_t *slot = &table[(h + i) & (REPORT_MAX_IDS - 1u)];
        if (!slot->used) {
            slot->used = 1;
            slot->id = id;
            slot->ext = ext ? 1 : 0;
            slot->count = 0;
            return slot;
        }
        if (slot->id == id && slot->ext == (ext ? 1 : 0)) {
            return slot;
        }
    }
    return NULL; /* more than REPORT_MAX_IDS distinct IDs: count as unknown */
}

static void fmt_count(char *buf, size_t cap, uint64_t n)
{
    if (n < 10000u) {
        snprintf(buf, cap, "%llu", (unsigned long long)n);
    } else if (n < 1000000u) {
        snprintf(buf, cap, "%.1fk", (double)n / 1000.0);
    } else {
        snprintf(buf, cap, "%.2fM", (double)n / 1000000.0);
    }
}

static void fmt_duration(char *buf, size_t cap, uint64_t ms)
{
    if (ms < 1000u) {
        snprintf(buf, cap, "%llu ms", (unsigned long long)ms);
    } else if (ms < 60000u) {
        snprintf(buf, cap, "%.2f s", (double)ms / 1000.0);
    } else if (ms < 3600000u) {
        snprintf(buf, cap, "%llu:%02llu", (unsigned long long)(ms / 60000u),
                 (unsigned long long)((ms % 60000u) / 1000u));
    } else {
        snprintf(buf, cap, "%llu:%02llu:%02llu", (unsigned long long)(ms / 3600000u),
                 (unsigned long long)((ms % 3600000u) / 60000u),
                 (unsigned long long)((ms % 60000u) / 1000u));
    }
}

static void fmt_wall(uint64_t wall_ms, bool valid, char *buf, size_t cap)
{
    if (!valid || wall_ms == 0) {
        snprintf(buf, cap, "uptime-only");
        return;
    }
    const time_t t = (time_t)(wall_ms / 1000u);
    struct tm tm;
    if (!localtime_r(&t, &tm)) {
        snprintf(buf, cap, "unknown");
        return;
    }
    strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tm);
}

static void html_escape(FILE *out, const char *s)
{
    for (; s && *s; s++) {
        switch (*s) {
        case '&':
            fputs("&amp;", out);
            break;
        case '<':
            fputs("&lt;", out);
            break;
        case '>':
            fputs("&gt;", out);
            break;
        case '"':
            fputs("&quot;", out);
            break;
        default:
            fputc(*s, out);
            break;
        }
    }
}

static int scan_once(const char *path, id_stat_t *table, report_ctx_t *ctx)
{
    lsc_reader_t reader;
    if (lsc_reader_open(&reader, path) != 0) {
        return -1;
    }
    ctx->bitrate = reader.bitrate;
    ctx->boot_count = reader.boot_count;
    ctx->flags = reader.flags;
    ctx->start_wall_ms = reader.start_wall_ms;
    ctx->wall_valid = reader.wall_valid;

    linsight_can_record_t rec;
    int rc;
    while ((rc = lsc_reader_next(&reader, &rec)) == 1) {
        const bool ext = (rec.flags & LINSIGHT_REC_FLAG_EXT) != 0;
        const bool rtr = (rec.flags & LINSIGHT_REC_FLAG_RTR) != 0;
        const bool err = (rec.flags & LINSIGHT_REC_FLAG_ERR) != 0;

        if (ctx->frames == 0) {
            ctx->first_ts = rec.ts_ms;
            ctx->last_ts = rec.ts_ms;
        } else {
            if (rec.ts_ms < ctx->first_ts) {
                ctx->first_ts = rec.ts_ms;
            }
            if (rec.ts_ms > ctx->last_ts) {
                ctx->last_ts = rec.ts_ms;
            }
        }
        ctx->frames++;
        if (ext) {
            ctx->ext_frames++;
        } else {
            ctx->std_frames++;
        }
        if (rtr) {
            ctx->rtr_frames++;
        }
        if (err) {
            ctx->err_frames++;
        }
        const uint32_t data_len =
            rtr ? 0u : (rec.dlc > LINSIGHT_DLC_MAX ? LINSIGHT_DLC_MAX : rec.dlc);
        ctx->data_bytes += data_len;
        /* Classic CAN: 47 fixed bits standard, 67 extended; + 8 data bits. */
        ctx->total_bits += (ext ? 67u : 47u) + data_len * 8u;

        if (!err) {
            id_stat_t *slot = id_get(table, rec.id, ext);
            if (slot) {
                slot->count++;
            } else {
                ctx->unknown_ids++;
            }
        }
    }
    lsc_reader_close(&reader);
    return rc < 0 ? -1 : 0;
}

static void top_insert(const id_stat_t *table, const id_stat_t **top, int *top_n)
{
    for (uint32_t i = 0; i < REPORT_MAX_IDS; i++) {
        const id_stat_t *slot = &table[i];
        if (!slot->used) {
            continue;
        }
        int pos = *top_n;
        if (pos < (int)REPORT_TOP) {
            (*top_n)++;
        } else {
            if (slot->count <= top[REPORT_TOP - 1]->count) {
                continue;
            }
            pos = (int)REPORT_TOP - 1;
        }
        while (pos > 0 &&
               (slot->count > top[pos - 1]->count ||
                (slot->count == top[pos - 1]->count &&
                 (slot->id < top[pos - 1]->id ||
                  (slot->id == top[pos - 1]->id && slot->ext < top[pos - 1]->ext))))) {
            top[pos] = top[pos - 1];
            pos--;
        }
        top[pos] = slot;
    }
}

int lsc_report_html(const char *lsc_path, const char *out_path, const char *title,
                    lsc_report_info_t *info)
{
    if (!lsc_path || !out_path) {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(out_path, "-") != 0 && strcmp(out_path, lsc_path) == 0) {
        errno = EINVAL;
        return -1;
    }

    id_stat_t *table = calloc(REPORT_MAX_IDS, sizeof(*table));
    if (!table) {
        errno = ENOMEM;
        return -1;
    }

    report_ctx_t ctx = {0};
    if (scan_once(lsc_path, table, &ctx) != 0) {
        free(table);
        return -1;
    }

    uint64_t duration_ms = ctx.last_ts >= ctx.first_ts ? ctx.last_ts - ctx.first_ts : 0;
    if (duration_ms == 0 && ctx.frames > 1) {
        duration_ms = 1;
    }

    uint64_t bucket_ms =
        duration_ms ? (duration_ms + REPORT_POINTS - 1u) / REPORT_POINTS : 1000u;
    if (bucket_ms < 1000u) {
        bucket_ms = 1000u;
    }
    uint32_t points = duration_ms ? (uint32_t)((duration_ms + bucket_ms - 1u) / bucket_ms) : 1u;
    if (points == 0) {
        points = 1;
    }
    if (points > REPORT_POINTS) {
        points = REPORT_POINTS;
    }
    uint32_t *timeline = calloc(points, sizeof(*timeline));
    if (!timeline) {
        free(table);
        errno = ENOMEM;
        return -1;
    }

    if (ctx.frames > 0) {
        lsc_reader_t reader;
        if (lsc_reader_open(&reader, lsc_path) == 0) {
            linsight_can_record_t rec;
            while (lsc_reader_next(&reader, &rec) == 1) {
                const uint64_t ts = rec.ts_ms < ctx.first_ts ? ctx.first_ts : rec.ts_ms;
                uint64_t idx = (ts - ctx.first_ts) / bucket_ms;
                if (idx >= points) {
                    idx = points - 1u;
                }
                timeline[idx]++;
            }
            lsc_reader_close(&reader);
        }
    }
    uint32_t peak = 0;
    for (uint32_t i = 0; i < points; i++) {
        if (timeline[i] > peak) {
            peak = timeline[i];
        }
    }
    if (peak == 0) {
        peak = 1;
    }

    uint64_t unique_ids = 0;
    for (uint32_t i = 0; i < REPORT_MAX_IDS; i++) {
        if (table[i].used) {
            unique_ids++;
        }
    }
    if (ctx.unknown_ids) {
        unique_ids++;
    }

    const id_stat_t *top[REPORT_TOP] = {0};
    int top_n = 0;
    top_insert(table, top, &top_n);

    FILE *out = strcmp(out_path, "-") == 0 ? stdout : fopen(out_path, "w");
    if (!out) {
        free(timeline);
        free(table);
        return -1;
    }

    const char *report_title = (title && *title) ? title : "LinSight CAN Report";
    char frames_buf[24];
    char duration_buf[24];
    char unique_buf[24];
    char rate_buf[24];
    char peak_buf[24];
    char load_buf[24];
    fmt_count(frames_buf, sizeof(frames_buf), ctx.frames);
    fmt_duration(duration_buf, sizeof(duration_buf), duration_ms);
    fmt_count(unique_buf, sizeof(unique_buf), unique_ids);
    const double duration_s = duration_ms ? (double)duration_ms / 1000.0 : 0.0;
    const double avg_rate = duration_s > 0.0 ? (double)ctx.frames / duration_s : 0.0;
    const double peak_rate = duration_s > 0.0 ? (double)peak * 1000.0 / (double)bucket_ms : 0.0;
    snprintf(rate_buf, sizeof(rate_buf), "%.1f/s", avg_rate);
    snprintf(peak_buf, sizeof(peak_buf), "%.0f/s", peak_rate);
    if (ctx.bitrate && duration_s > 0.0) {
        const double load =
            (double)ctx.total_bits * 100.0 / ((double)ctx.bitrate * duration_s);
        snprintf(load_buf, sizeof(load_buf), "%.1f%%", load);
    } else {
        snprintf(load_buf, sizeof(load_buf), "--");
    }

    fputs("<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n<title>",
          out);
    html_escape(out, report_title);
    fputs("</title><style>\n"
          ":root{color-scheme:light dark;--bg:#f6f7f9;--card:#fff;--ink:#111;--dim:#69707a;"
          "--line:#e3e6ea;--blue:#0a84ff;}\n"
          "@media (prefers-color-scheme:dark){:root{--bg:#0b0c0e;--card:#15171b;--ink:#f4f6f8;"
          "--dim:#9aa1ab;--line:#272b31;--blue:#4da3ff;}}\n"
          "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);"
          "font:14px/1.5 -apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif}\n"
          "main{max-width:980px;margin:0 auto;padding:32px 20px 48px}\n"
          "h1{font-size:26px;margin:0 0 4px}h2{font-size:15px;letter-spacing:.06em;"
          "text-transform:uppercase;color:var(--dim);margin:32px 0 12px}\n"
          ".sub{color:var(--dim);margin:0 0 24px;word-break:break-all}\n"
          ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px}\n"
          ".tile{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px}\n"
          ".tile .k{font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--dim)}\n"
          ".tile .v{font-size:22px;font-weight:650;margin-top:4px;font-variant-numeric:tabular-nums}\n"
          ".panel{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px}\n"
          "table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}\n"
          "th,td{text-align:right;padding:9px 8px;border-bottom:1px solid var(--line)}\n"
          "th:first-child,td:first-child{text-align:left}th{font-size:11px;letter-spacing:.06em;"
          "text-transform:uppercase;color:var(--dim);font-weight:600}\n"
          "tr:last-child td{border-bottom:0}.bar{height:6px;border-radius:6px;background:var(--line);"
          "overflow:hidden;min-width:70px}.bar>i{display:block;height:100%;background:var(--blue)}\n"
          ".meta{display:flex;flex-wrap:wrap;gap:8px 22px;color:var(--dim);font-size:13px}\n"
          ".meta b{color:var(--ink);font-weight:600}.mono{font-family:ui-monospace,SFMono-Regular,"
          "Consolas,monospace}.empty{padding:40px;text-align:center;color:var(--dim)}\n"
          "footer{margin-top:32px;color:var(--dim);font-size:12px}\n"
          "</style></head><body><main>\n",
          out);

    fputs("<h1>", out);
    html_escape(out, report_title);
    fputs("</h1><p class=\"sub\">", out);
    html_escape(out, lsc_path);
    fputs(" &middot; generated ", out);
    const time_t now = time(NULL);
    struct tm now_tm;
    if (localtime_r(&now, &now_tm)) {
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &now_tm);
        fputs(stamp, out);
    }
    fputs("</p>\n", out);

    fputs("<section class=\"grid\">", out);
    const struct {
        const char *key;
        const char *value;
    } tiles[] = {
        {"Frames", frames_buf},       {"Duration", duration_buf},
        {"Unique IDs", unique_buf},   {"Average rate", rate_buf},
        {"Peak rate", peak_buf},      {"Bus load (est.)", load_buf},
    };
    for (size_t i = 0; i < sizeof(tiles) / sizeof(tiles[0]); i++) {
        fputs("<div class=\"tile\"><div class=\"k\">", out);
        fputs(tiles[i].key, out);
        fputs("</div><div class=\"v\">", out);
        fputs(tiles[i].value, out);
        fputs("</div></div>", out);
    }
    fputs("</section>\n", out);

    fputs("<h2>Frames per second</h2><div class=\"panel\">", out);
    if (ctx.frames == 0) {
        fputs("<div class=\"empty\">No records in this log.</div>", out);
    } else {
        fputs("<svg viewBox=\"0 0 800 140\" width=\"100%\" height=\"180\" role=\"img\" "
              "aria-label=\"frames per second\">", out);
        fputs("<line x1=\"10\" y1=\"120\" x2=\"790\" y2=\"120\" stroke=\"currentColor\" "
              "stroke-opacity=\".25\"/><polyline fill=\"none\" stroke=\"#0a84ff\" "
              "stroke-width=\"2\" stroke-linejoin=\"round\" points=\"",
              out);
        for (uint32_t i = 0; i < points; i++) {
            const int x =
                points == 1 ? 400 : 10 + (int)((uint64_t)i * 780u / (points - 1u));
            /* Buckets can span more than one second on long captures; scale to
             * frames per second so the axis matches the chart title. */
            const double bucket_fps = (double)timeline[i] * 1000.0 / (double)bucket_ms;
            const double scale = peak_rate > 0.0 ? peak_rate : 1.0;
            const int y = 120 - (int)(bucket_fps * 100.0 / scale);
            fprintf(out, "%d,%d ", x, y);
        }
        fputs("\"/></svg><div class=\"meta\"><span>peak <b>", out);
        fputs(peak_buf, out);
        fputs("</b></span></div>", out);
    }
    fputs("</div>\n", out);

    fputs("<h2>Top IDs</h2><div class=\"panel\">", out);
    if (top_n == 0) {
        fputs("<div class=\"empty\">No data frames.</div>", out);
    } else {
        fputs("<table><thead><tr><th>ID</th><th>Frames</th><th>Avg rate</th>"
              "<th>Share</th><th></th></tr></thead><tbody>",
              out);
        for (int i = 0; i < top_n; i++) {
            char idbuf[16];
            if (top[i]->ext) {
                snprintf(idbuf, sizeof(idbuf), "0x%08X", (unsigned)top[i]->id);
            } else {
                snprintf(idbuf, sizeof(idbuf), "0x%03X", (unsigned)top[i]->id);
            }
            char count_buf[24];
            fmt_count(count_buf, sizeof(count_buf), top[i]->count);
            const double rate = duration_s > 0.0 ? (double)top[i]->count / duration_s : 0.0;
            const double share =
                ctx.frames ? (double)top[i]->count * 100.0 / (double)ctx.frames : 0.0;
            const uint64_t width =
                top[0]->count ? top[i]->count * 100u / top[0]->count : 0;
            fprintf(out,
                    "<tr><td class=\"mono\">%s%s</td><td>%s</td><td>%.1f/s</td>"
                    "<td>%.1f%%</td><td><div class=\"bar\"><i style=\"width:%llu%%\"></i>"
                    "</div></td></tr>",
                    idbuf, top[i]->ext ? " EXT" : "", count_buf, rate, share,
                    (unsigned long long)width);
        }
        fputs("</tbody></table>", out);
    }
    fputs("</div>\n", out);

    char start_buf[40];
    char end_buf[40];
    if (ctx.wall_valid) {
        fmt_wall(ctx.start_wall_ms, true, start_buf, sizeof(start_buf));
        fmt_wall(ctx.start_wall_ms + ctx.last_ts, true, end_buf, sizeof(end_buf));
    } else {
        snprintf(start_buf, sizeof(start_buf), "uptime %llu ms",
                 (unsigned long long)ctx.first_ts);
        snprintf(end_buf, sizeof(end_buf), "uptime %llu ms",
                 (unsigned long long)ctx.last_ts);
    }
    char data_buf[24];
    char file_buf[24];
    fmt_count(data_buf, sizeof(data_buf), ctx.data_bytes);
    fmt_count(file_buf, sizeof(file_buf),
              (uint64_t)LSC_HEADER_SIZE + ctx.frames * LINSIGHT_REC_SIZE);

    fputs("<h2>Session</h2><div class=\"panel\"><div class=\"meta\">", out);
    fprintf(out, "<span>Start <b>%s</b></span><span>End <b>%s</b></span>", start_buf,
            end_buf);
    fprintf(out, "<span>Bitrate <b>%u kbit/s</b></span>", ctx.bitrate / 1000u);
    fprintf(out, "<span>Standard <b>%llu</b></span>",
            (unsigned long long)ctx.std_frames);
    fprintf(out, "<span>Extended <b>%llu</b></span>",
            (unsigned long long)ctx.ext_frames);
    fprintf(out, "<span>RTR <b>%llu</b></span><span>Error <b>%llu</b></span>",
            (unsigned long long)ctx.rtr_frames, (unsigned long long)ctx.err_frames);
    fprintf(out, "<span>Data <b>%s B</b></span><span>Log <b>%s B</b></span>", data_buf,
            file_buf);
    if (ctx.flags & LSC_FLAG_BOOT_VALID) {
        fprintf(out, "<span>Boot <b>%u</b></span>", ctx.boot_count);
    }
    fputs("</div></div>\n", out);

    fputs("<footer>Generated by <span class=\"mono\">linsight-cli report</span>.</footer>\n"
          "</main></body></html>\n",
          out);

    if (out != stdout && fclose(out) != 0) {
        free(timeline);
        free(table);
        return -1;
    }
    if (info) {
        info->frames = ctx.frames;
        info->unique_ids = unique_ids;
        info->start_ts_ms = ctx.first_ts;
        info->end_ts_ms = ctx.last_ts;
    }
    free(timeline);
    free(table);
    return 0;
}
