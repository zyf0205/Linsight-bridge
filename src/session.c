#include "session.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *find_key(const char *json, const char *key)
{
    char pattern[48];
    const int n = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    if (n <= 0 || n >= (int)sizeof(pattern)) {
        return NULL;
    }
    return strstr(json, pattern);
}

static bool json_u32(const char *json, const char *key, uint32_t *out)
{
    const char *p = find_key(json, key);
    if (!p) {
        return false;
    }
    p += strlen(key) + 3; /* "key": */
    while (*p == ' ') {
        p++;
    }
    if (!isdigit((unsigned char)*p)) {
        return false;
    }
    char *end = NULL;
    const unsigned long v = strtoul(p, &end, 10);
    if (end == p) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool json_str(const char *json, const char *key, char *out, size_t cap)
{
    const char *p = find_key(json, key);
    if (!p) {
        return false;
    }
    p += strlen(key) + 3;
    while (*p == ' ') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
        }
        if (i + 1 < cap) {
            out[i++] = *p;
        }
        p++;
    }
    if (*p != '"') {
        return false;
    }
    out[i] = '\0';
    return true;
}

bool session_parse_hello(const uint8_t *payload, uint16_t len, session_hello_t *out)
{
    if (!payload || !out || len == 0 || len > LINSIGHT_MAX_HELLO) {
        return false;
    }
    char json[LINSIGHT_MAX_HELLO + 1];
    memcpy(json, payload, len);
    json[len] = '\0';

    memset(out, 0, sizeof(*out));
    out->proto = LINSIGHT_PROTO_VERSION;
    (void)json_u32(json, "proto", &out->proto);
    (void)json_str(json, "fw", out->fw, sizeof(out->fw));
    out->has_device_id = json_str(json, "device_id", out->device_id, sizeof(out->device_id));
    out->has_boot = json_u32(json, "boot", &out->boot_count);
    out->has_uptime = json_u32(json, "uptime_ms", &out->uptime_ms);

    char br[16] = {0};
    if (json_str(json, "bitrate", br, sizeof(br))) {
        if (strcmp(br, "125k") == 0) {
            out->bitrate = 125000;
        } else if (strcmp(br, "250k") == 0) {
            out->bitrate = 250000;
        } else if (strcmp(br, "1M") == 0) {
            out->bitrate = 1000000;
        } else if (strcmp(br, "500k") == 0) {
            out->bitrate = 500000;
        }
    }
    return true;
}

uint64_t session_anchor_start_wall_ms(uint64_t host_wall_ms, uint32_t uptime_ms)
{
    return host_wall_ms > uptime_ms ? host_wall_ms - uptime_ms : 0;
}

void session_clear_counters(session_state_t *s)
{
    s->accepted = 0;
    s->skipped = 0;
    s->gaps = 0;
    s->missing = 0;
    s->ambiguous = 0;
}

void session_reset(session_state_t *s, uint32_t boot_count, bool have_boot)
{
    if (!have_boot) {
        /* Old firmware identifies no boot: a new HELLO is a new session, so
         * its records must not be judged against the previous device's
         * marks. Reconnect dedupe is only possible when boot is known. */
        s->have_mark = false;
    } else if (s->have_boot && boot_count != s->boot_count) {
        s->have_mark = false;
    }
    s->boot_count = boot_count;
    s->have_boot = have_boot;
}

bool session_record_new(session_state_t *s, const linsight_can_record_t *rec)
{
    if (!s->have_mark) {
        s->have_mark = true;
        s->last_ts_ms = rec->ts_ms;
        s->last_seq = rec->seq;
        s->accepted++;
        return true;
    }

    if (rec->ts_ms < s->last_ts_ms) {
        s->skipped++;
        return false;
    }

    /* Modular difference: wrap 0xFFFF -> 0 is +1, a duplicate is <= 0. */
    const int32_t diff = (int16_t)(rec->seq - s->last_seq);

    if (rec->ts_ms == s->last_ts_ms) {
        if (diff == 0) {
            s->skipped++;
            return false;
        }
        if (diff < 0 && diff > -1024) {
            s->skipped++;
            return false;
        }
    } else if (diff == 0) {
        s->ambiguous++;
    } else if (diff < 0) {
        s->ambiguous++;
    } else if (diff > 1) {
        s->gaps++;
        s->missing += (uint32_t)(diff - 1);
    }

    s->last_ts_ms = rec->ts_ms;
    s->last_seq = rec->seq;
    s->accepted++;
    return true;
}
