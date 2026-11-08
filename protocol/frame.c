#include "frame.h"

#include <string.h>

uint16_t linsight_frame_max_payload(uint8_t type)
{
    switch (type) {
    case LINSIGHT_FRAME_HELLO:
        return LINSIGHT_MAX_HELLO;
    case LINSIGHT_FRAME_CAN_BATCH:
        return LINSIGHT_MAX_CAN_BATCH;
    case LINSIGHT_FRAME_STATS:
        return LINSIGHT_MAX_STATS;
    case LINSIGHT_FRAME_EVENT:
        return LINSIGHT_MAX_EVENT;
    case LINSIGHT_FRAME_PING:
        return LINSIGHT_MAX_PING;
    case LINSIGHT_FRAME_PONG:
        return LINSIGHT_MAX_PONG;
    case LINSIGHT_FRAME_CONTROL:
        return LINSIGHT_MAX_CONTROL;
    case LINSIGHT_FRAME_ACK:
        return LINSIGHT_MAX_ACK;
    case LINSIGHT_FRAME_FLOW:
        return LINSIGHT_MAX_FLOW;
    case LINSIGHT_FRAME_DEVICE_INFO:
        return LINSIGHT_MAX_DEVICE_INFO;
    default:
        return UINT16_MAX;
    }
}

void linsight_frame_write_header(uint8_t *dst, uint8_t type, uint16_t len)
{
    dst[0] = LINSIGHT_FRAME_MAGIC0;
    dst[1] = LINSIGHT_FRAME_MAGIC1;
    dst[2] = type;
    dst[3] = (uint8_t)(len & 0xFFu);
    dst[4] = (uint8_t)((len >> 8) & 0xFFu);
}

size_t linsight_frame_build(uint8_t *dst, size_t cap, uint8_t type, const void *payload,
                            uint16_t len)
{
    const size_t need = LINSIGHT_FRAME_HEADER_SIZE + (size_t)len;

    if (!dst || cap < need || (len > 0 && !payload)) {
        return 0;
    }
    if (len > linsight_frame_max_payload(type)) {
        return 0;
    }

    linsight_frame_write_header(dst, type, len);
    if (len > 0) {
        memcpy(dst + LINSIGHT_FRAME_HEADER_SIZE, payload, len);
    }
    return need;
}

void linsight_record_pack(const linsight_can_record_t *r, uint8_t out[LINSIGHT_REC_SIZE])
{
    linsight_put_u16(out + 0, r->seq);
    linsight_put_u32(out + 2, r->ts_ms);
    linsight_put_u32(out + 6, r->id);
    out[10] = r->flags;
    out[11] = r->dlc;
    memcpy(out + 12, r->data, LINSIGHT_DLC_MAX);
}

bool linsight_record_unpack(const uint8_t in[LINSIGHT_REC_SIZE], linsight_can_record_t *r)
{
    r->seq = linsight_get_u16(in + 0);
    r->ts_ms = linsight_get_u32(in + 2);
    r->id = linsight_get_u32(in + 6);
    r->flags = in[10];
    r->dlc = in[11];
    memcpy(r->data, in + 12, LINSIGHT_DLC_MAX);
    return r->dlc <= LINSIGHT_DLC_MAX;
}

size_t linsight_batch_pack(uint8_t *dst, size_t cap, const linsight_can_record_t *recs,
                           uint16_t count)
{
    if (!dst || count == 0 || count > LINSIGHT_BATCH_MAX_RECORDS || !recs) {
        return 0;
    }

    const size_t need = 2u + (size_t)count * LINSIGHT_REC_SIZE;
    if (cap < need || need > LINSIGHT_MAX_CAN_BATCH) {
        return 0;
    }

    linsight_put_u16(dst, count);
    for (uint16_t i = 0; i < count; i++) {
        linsight_record_pack(&recs[i], dst + 2 + (size_t)i * LINSIGHT_REC_SIZE);
    }
    return need;
}

static void parser_clear_frame(linsight_parser_t *p)
{
    p->hdr_len = 0;
    p->type = 0;
    p->payload_len = 0;
    p->payload_got = 0;
    p->skip_payload = false;
}

void linsight_parser_init(linsight_parser_t *p, uint8_t *payload_buf, uint16_t payload_cap)
{
    if (!p) {
        return;
    }
    memset(p, 0, sizeof(*p));
    p->payload_buf = payload_buf;
    p->payload_cap = payload_buf ? payload_cap : 0;
}

void linsight_parser_reset(linsight_parser_t *p)
{
    if (!p) {
        return;
    }
    /* Preserve the caller's payload buffer/capacity; clear state and counters. */
    parser_clear_frame(p);
    p->fatal = false;
    p->frames = 0;
    p->skipped = 0;
    p->resyncs = 0;
}

static void parser_begin_payload(linsight_parser_t *p)
{
    p->type = p->hdr[2];
    p->payload_len = linsight_get_u16(&p->hdr[3]);
    p->payload_got = 0;

    const uint16_t max = linsight_frame_max_payload(p->type);
    if (max != UINT16_MAX && p->payload_len > max) {
        p->fatal = true;
        return;
    }

    p->skip_payload = (max == UINT16_MAX) || (p->payload_len > p->payload_cap);
}

static void parser_finish_frame(linsight_parser_t *p, linsight_frame_cb_t cb, void *ctx)
{
    const uint8_t type = p->type;
    const uint16_t len = p->payload_len;

    if (!p->skip_payload && cb) {
        cb(ctx, type, len ? p->payload_buf : NULL, len);
    }
    p->frames++;
    parser_clear_frame(p);
}

linsight_parse_result_t linsight_parser_feed(linsight_parser_t *p, const uint8_t *data,
                                             size_t len, linsight_frame_cb_t cb, void *ctx)
{
    if (!p || (!data && len)) {
        return LINSIGHT_PARSE_ERROR;
    }
    if (p->fatal) {
        return LINSIGHT_PARSE_ERROR;
    }

    for (size_t i = 0; i < len; i++) {
        const uint8_t b = data[i];

        if (p->hdr_len < 2) {
            if (p->hdr_len == 0) {
                if (b == LINSIGHT_FRAME_MAGIC0) {
                    p->hdr[p->hdr_len++] = b;
                } else {
                    p->resyncs++;
                }
            } else { /* hdr_len == 1, waiting for 0x55 */
                if (b == LINSIGHT_FRAME_MAGIC1) {
                    p->hdr[p->hdr_len++] = b;
                } else if (b == LINSIGHT_FRAME_MAGIC0) {
                    p->resyncs++;
                } else {
                    p->resyncs++;
                    p->hdr_len = 0;
                }
            }
            continue;
        }

        if (p->hdr_len < LINSIGHT_FRAME_HEADER_SIZE) {
            p->hdr[p->hdr_len++] = b;
            if (p->hdr_len == LINSIGHT_FRAME_HEADER_SIZE) {
                parser_begin_payload(p);
                if (p->fatal) {
                    return LINSIGHT_PARSE_ERROR;
                }
                if (p->payload_len == 0) {
                    parser_finish_frame(p, cb, ctx);
                }
            }
            continue;
        }

        /* Payload byte. */
        p->payload_got++;
        if (!p->skip_payload) {
            p->payload_buf[p->payload_got - 1] = b;
        }
        if (p->payload_got == p->payload_len) {
            if (p->skip_payload) {
                p->skipped++;
                parser_clear_frame(p);
            } else {
                parser_finish_frame(p, cb, ctx);
            }
        }
    }

    return LINSIGHT_PARSE_OK;
}
