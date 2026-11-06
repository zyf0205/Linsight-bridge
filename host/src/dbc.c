#define _POSIX_C_SOURCE 200809L

#include "dbc.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *skip_ws(char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static dbc_message_t *parse_bo(dbc_t *dbc, char *p)
{
    unsigned long raw = 0;
    char name[DBC_NAME_MAX] = {0};
    unsigned dlc = 0;
    if (dbc->count >= DBC_MAX_MESSAGES ||
        sscanf(p, "BO_ %lu %63s %u", &raw, name, &dlc) != 3) {
        return NULL;
    }

    size_t n = strlen(name);
    if (n > 0 && name[n - 1] == ':') {
        name[n - 1] = '\0';
    }

    dbc_message_t *m = &dbc->messages[dbc->count++];
    m->ext = (raw & 0x80000000ul) ? 1 : 0;
    m->id = m->ext ? (uint32_t)(raw & 0x1FFFFFFFul) : (uint32_t)raw;
    m->dlc = dlc > 8 ? 8 : (uint8_t)dlc;
    m->signal_count = 0;
    snprintf(m->name, sizeof(m->name), "%s", name);
    return m;
}

static void parse_sg(dbc_message_t *m, char *p)
{
    if (!m || m->signal_count >= DBC_MAX_SIGNALS) {
        return;
    }

    char name[DBC_NAME_MAX] = {0};
    int used = 0;
    if (sscanf(p, "SG_ %63s%n", name, &used) != 1) {
        return;
    }
    p = skip_ws(p + used);

    /* Some exporters put the colon directly after the name ("Rpm:"); the
     * %s above then consumed it. */
    size_t name_len = strlen(name);
    const bool colon_consumed = name_len > 0 && name[name_len - 1] == ':';
    if (colon_consumed) {
        name[name_len - 1] = '\0';
    }

    if (!colon_consumed) {
        /* A second token before ':' is a multiplex indicator: "M" marks the
         * selector itself, "mN" is conditional and skipped. */
        if (*p != ':') {
            char mux[32] = {0};
            used = 0;
            if (sscanf(p, "%31s%n", mux, &used) != 1) {
                return;
            }
            p = skip_ws(p + used);
            if (mux[0] == 'm') {
                return;
            }
        }
        if (*p != ':') {
            return;
        }
        p++;
    }

    unsigned start = 0, length = 0;
    char order = 0, sign = 0;
    double factor = 0, offset = 0, min = 0, max = 0;
    const int n = sscanf(p, " %u | %u @ %c %c ( %lf , %lf ) [ %lf | %lf ]",
                         &start, &length, &order, &sign, &factor, &offset, &min, &max);
    if (n < 6 || length == 0 || length > 64 || start > 63) {
        return;
    }

    dbc_signal_t *s = &m->signals[m->signal_count++];
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->start = (uint16_t)start;
    s->length = (uint16_t)length;
    s->little_endian = order == '1';
    s->is_signed = sign == '-';
    s->factor = factor;
    s->offset = offset;
    s->unit[0] = '\0';

    const char *quote = strchr(p, '"');
    if (quote) {
        quote++;
        const char *end = strchr(quote, '"');
        if (end) {
            size_t len = (size_t)(end - quote);
            if (len >= sizeof(s->unit)) {
                len = sizeof(s->unit) - 1;
            }
            memcpy(s->unit, quote, len);
            s->unit[len] = '\0';
        }
    }
}

int dbc_load(dbc_t *dbc, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    memset(dbc, 0, sizeof(*dbc));

    dbc_message_t *current = NULL;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) >= 0) {
        char *p = skip_ws(line);
        if (strncmp(p, "BO_ ", 4) == 0) {
            current = parse_bo(dbc, p);
        } else if (strncmp(p, "SG_ ", 4) == 0) {
            parse_sg(current, p);
        }
    }
    free(line);
    fclose(f);
    return 0;
}

const dbc_message_t *dbc_find(const dbc_t *dbc, uint32_t id, int ext)
{
    const int want_ext = ext ? 1 : 0;
    for (size_t i = 0; i < dbc->count; i++) {
        if (dbc->messages[i].id == id && dbc->messages[i].ext == want_ext) {
            return &dbc->messages[i];
        }
    }
    return NULL;
}

static int extract_le(const dbc_signal_t *s, const uint8_t *data, unsigned total,
                      uint64_t *raw)
{
    if ((unsigned)s->start + s->length > total) {
        return -1;
    }
    uint64_t value = 0;
    for (unsigned i = 0; i < s->length; i++) {
        const unsigned bit = s->start + i;
        if (data[bit >> 3] & (1u << (bit & 7u))) {
            value |= 1ull << i;
        }
    }
    *raw = value;
    return 0;
}

static int extract_be(const dbc_signal_t *s, const uint8_t *data, unsigned total,
                      uint64_t *raw)
{
    uint64_t value = 0;
    unsigned bit = s->start;
    for (unsigned i = 0; i < s->length; i++) {
        if (bit >= total) {
            return -1;
        }
        value = (value << 1) | ((data[bit >> 3] >> (bit & 7u)) & 1u);
        if ((bit & 7u) == 0) {
            bit += 15;
        } else {
            bit--;
        }
    }
    *raw = value;
    return 0;
}

int dbc_decode_signal(const dbc_signal_t *sig, const uint8_t *data, uint8_t dlc,
                      double *out)
{
    const unsigned total = (unsigned)dlc * 8u;
    uint64_t raw = 0;
    if (sig->little_endian ? extract_le(sig, data, total, &raw)
                           : extract_be(sig, data, total, &raw)) {
        return -1;
    }

    double value;
    if (sig->is_signed) {
        if (sig->length < 64 && (raw & (1ull << (sig->length - 1)))) {
            raw |= ~0ull << sig->length;
        }
        value = (double)(int64_t)raw;
    } else {
        value = (double)raw;
    }
    *out = value * sig->factor + sig->offset;
    return 0;
}
