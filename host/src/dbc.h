#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Minimal DBC reader for offline decoding. Supports BO_ messages and
 * non-multiplexed SG_ signals (Intel/Motorola, signed/unsigned, factor and
 * offset). Multiplexed signals (m0, m1, ...) are skipped; the multiplexor
 * signal itself (M) is kept. Value tables (VAL_) and attributes are ignored.
 */

#define DBC_MAX_MESSAGES 256
#define DBC_MAX_SIGNALS 32
#define DBC_NAME_MAX 64
#define DBC_UNIT_MAX 24

typedef struct {
    char name[DBC_NAME_MAX];
    char unit[DBC_UNIT_MAX];
    uint16_t start;
    uint16_t length;
    uint8_t little_endian; /* 1 = Intel (@1), 0 = Motorola (@0) */
    uint8_t is_signed;
    double factor;
    double offset;
} dbc_signal_t;

typedef struct {
    uint32_t id; /* 11-bit or 29-bit CAN ID, flag stripped */
    uint8_t ext;
    uint8_t dlc;
    char name[DBC_NAME_MAX];
    size_t signal_count;
    dbc_signal_t signals[DBC_MAX_SIGNALS];
} dbc_message_t;

typedef struct {
    size_t count;
    dbc_message_t messages[DBC_MAX_MESSAGES];
} dbc_t;

/* Parse a .dbc file. Returns 0 on success (unknown lines are ignored), or -1
 * when the file cannot be opened. */
int dbc_load(dbc_t *dbc, const char *path);

/* Find a message by CAN ID and frame kind. Returns NULL when absent. */
const dbc_message_t *dbc_find(const dbc_t *dbc, uint32_t id, int ext);

/* Decode one signal from a frame. Returns 0 on success, -1 when the signal
 * does not fit in the frame's DLC. */
int dbc_decode_signal(const dbc_signal_t *sig, const uint8_t *data, uint8_t dlc,
                      double *out);
