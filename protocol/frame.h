#pragma once

/*
 * LinSight wire protocol v2.0 (CAN analyzer), shared by the ESP32-S3 device
 * and the Linux host.
 *
 * This file is intentionally free of ESP-IDF, POSIX and libc-allocation
 * dependencies: both builds compile the same parser, and the host test suite
 * can fuzz it without a board.
 *
 * Frame: AA 55 | type | len_lo | len_hi | payload[len]
 * All multi-byte integers are little-endian.
 *
 * Device -> host: HELLO, CAN_BATCH, STATS, EVENT, PING, ACK, DEVICE_INFO
 * Host -> device: CONTROL, FLOW, PING, PONG
 * CAN_BATCH payload: u16 count + count * 20-byte records (see below).
 * STATS / EVENT / HELLO / CONTROL / ACK / DEVICE_INFO payloads are small JSON
 * objects. DEVICE_INFO is additive (type 10); older hosts skip unknown types.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINSIGHT_FRAME_MAGIC0 0xAAu
#define LINSIGHT_FRAME_MAGIC1 0x55u
#define LINSIGHT_FRAME_HEADER_SIZE 5u

#define LINSIGHT_PROTO_VERSION 0x21u

#define LINSIGHT_MAX_HELLO 256u
#define LINSIGHT_MAX_CAN_BATCH 16384u
#define LINSIGHT_MAX_STATS 512u
#define LINSIGHT_MAX_EVENT 128u
#define LINSIGHT_MAX_PING 4u
#define LINSIGHT_MAX_PONG 4u
#define LINSIGHT_MAX_CONTROL 128u
#define LINSIGHT_MAX_ACK 32u /* {"id":N,"code":N} needs 17+ bytes */
#define LINSIGHT_MAX_FLOW 1u
#define LINSIGHT_MAX_DEVICE_INFO 512u

/* DEVICE_INFO "caps" bitmask. */
#define LINSIGHT_CAP_SD 0x01u
#define LINSIGHT_CAP_MARK 0x02u
#define LINSIGHT_CAP_PAUSE_LEASE 0x04u

/* Host-initiated capture pause lease. A dead host must not stop capture
 * forever; the device resumes after the lease (or on disconnect). */
#define LINSIGHT_PAUSE_LEASE_DEFAULT_MS 300000u
#define LINSIGHT_PAUSE_LEASE_MIN_MS 5000u
#define LINSIGHT_PAUSE_LEASE_MAX_MS 1800000u

/* One 20-byte record, wire order:
 * seq u16 | ts_ms u32 | id u32 | flags u8 | dlc u8 | data[8]
 *
 * The in-memory fields are ordered to avoid struct padding so sizeof() equals
 * LINSIGHT_REC_SIZE; linsight_record_pack/unpack define the wire order and must
 * not be replaced with a raw struct copy. */
#define LINSIGHT_REC_SIZE 20u
#define LINSIGHT_REC_FLAG_EXT 0x01u
#define LINSIGHT_REC_FLAG_RTR 0x02u
#define LINSIGHT_REC_FLAG_ERR 0x04u
#define LINSIGHT_DLC_MAX 8u

/* Max records in one CAN_BATCH payload (payload = 2 + count * 20 <= 16384). */
#define LINSIGHT_BATCH_MAX_RECORDS 819u

typedef enum {
    LINSIGHT_FRAME_HELLO = 1,
    LINSIGHT_FRAME_CAN_BATCH = 2,
    LINSIGHT_FRAME_STATS = 3,
    LINSIGHT_FRAME_EVENT = 4,
    LINSIGHT_FRAME_PING = 5,
    LINSIGHT_FRAME_PONG = 6,
    LINSIGHT_FRAME_CONTROL = 7,
    LINSIGHT_FRAME_ACK = 8,
    LINSIGHT_FRAME_FLOW = 9,
    LINSIGHT_FRAME_DEVICE_INFO = 10,
} linsight_frame_type_t;

/* CONTROL actions (JSON "action" field). */
#define LINSIGHT_CTRL_PAUSE "pause"   /* + optional lease_ms */
#define LINSIGHT_CTRL_RESUME "resume"
#define LINSIGHT_CTRL_CLEAR "clear_stats"
#define LINSIGHT_CTRL_CONFIG "set_config" /* + optional bitrate / mode */
#define LINSIGHT_CTRL_MARK "mark"         /* + optional id / seq: SD trigger mark */

typedef enum {
    LINSIGHT_ACK_OK = 0,
    LINSIGHT_ACK_BAD_ARG = 2,
    LINSIGHT_ACK_FAILED = 3,
} linsight_ack_code_t;

typedef struct {
    uint32_t ts_ms;
    uint32_t id;
    uint16_t seq;
    uint8_t flags; /* LINSIGHT_REC_FLAG_* */
    uint8_t dlc;
    uint8_t data[LINSIGHT_DLC_MAX];
} linsight_can_record_t;

#if defined(__cplusplus)
static_assert(sizeof(linsight_can_record_t) == LINSIGHT_REC_SIZE,
              "CAN record must stay 20 bytes in memory");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(linsight_can_record_t) == LINSIGHT_REC_SIZE,
               "CAN record must stay 20 bytes in memory");
#endif

static inline uint16_t linsight_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t linsight_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline void linsight_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void linsight_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Maximum payload for a known type, or 0xFFFF for an unknown type. */
uint16_t linsight_frame_max_payload(uint8_t type);

/* Write a 5-byte header. dst must have room for LINSIGHT_FRAME_HEADER_SIZE. */
void linsight_frame_write_header(uint8_t *dst, uint8_t type, uint16_t len);

/*
 * Build a complete frame into dst. Returns the frame size, or 0 when dst is
 * too small or the payload exceeds the type's limit. payload may be NULL only
 * when len is 0.
 */
size_t linsight_frame_build(uint8_t *dst, size_t cap, uint8_t type, const void *payload,
                            uint16_t len);

/* Serialize one record (little-endian, no padding). */
void linsight_record_pack(const linsight_can_record_t *r, uint8_t out[LINSIGHT_REC_SIZE]);

/*
 * Deserialize one record. Returns false when dlc > 8; the struct is still
 * filled in so the caller can log it.
 */
bool linsight_record_unpack(const uint8_t in[LINSIGHT_REC_SIZE], linsight_can_record_t *r);

/*
 * Build a CAN_BATCH payload (u16 count + packed records) into dst.
 * Returns the payload size, or 0 when dst is too small / count is invalid.
 */
size_t linsight_batch_pack(uint8_t *dst, size_t cap, const linsight_can_record_t *recs,
                           uint16_t count);

/*
 * Incremental parser. The caller supplies the payload scratch buffer (the
 * parser never allocates). Frames larger than payload_cap are skipped rather
 * than copied; the callback is not invoked for them. This lets a host parser
 * ignore CAN_BATCH frames with a small scratch buffer.
 *
 * feed() may be called with arbitrary fragmentation and with any number of
 * frames per call. It returns LINSIGHT_PARSE_ERROR after a known type exceeds
 * its protocol limit; the caller must close the connection. A partial frame at
 * the end of a call is normal and returns LINSIGHT_PARSE_OK.
 */
typedef enum {
    LINSIGHT_PARSE_OK = 0,
    LINSIGHT_PARSE_ERROR = -1,
} linsight_parse_result_t;

typedef void (*linsight_frame_cb_t)(void *ctx, uint8_t type, const uint8_t *payload,
                                    uint16_t len);

typedef struct {
    uint8_t hdr[LINSIGHT_FRAME_HEADER_SIZE];
    uint8_t hdr_len;
    uint8_t type;
    uint16_t payload_len;
    uint16_t payload_got;
    uint8_t *payload_buf;
    uint16_t payload_cap;
    bool skip_payload;
    bool fatal;

    uint32_t frames;
    uint32_t skipped;
    uint32_t resyncs;
} linsight_parser_t;

void linsight_parser_init(linsight_parser_t *p, uint8_t *payload_buf, uint16_t payload_cap);

/* Drop any partial frame. Also clears the fatal flag and statistics. */
void linsight_parser_reset(linsight_parser_t *p);

linsight_parse_result_t linsight_parser_feed(linsight_parser_t *p, const uint8_t *data,
                                             size_t len, linsight_frame_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
