/*
 * Device session: TCP accept, v2.1 frame dispatch, reconnect dedupe, PONG TX.
 */

#define _DEFAULT_SOURCE

#include "device.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "epoll_tags.h"
#include "frame.h"
#include "pacer.h"
#include "util.h"
#include "vcan.h"

#define TAG "linsightd"

#define RX_BUF_SIZE 4096
#define IDLE_TIMEOUT_SEC 30
#define DEV_OUT_CAP 64u

/* ------------------------------------------------ session & deduplication */

typedef struct {
    uint32_t boot_count;
    bool have_boot;
    bool have_mark;
    uint32_t last_ts_ms;
    uint16_t last_seq;
} session_state_t;

static bool json_u32(const char *json, const char *key, uint32_t *out)
{
    char pattern[32];
    const int n = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    if (n <= 0 || n >= (int)sizeof(pattern)) {
        return false;
    }
    const char *p = strstr(json, pattern);
    if (!p) {
        return false;
    }
    p += n;
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

static void session_reset(session_state_t *s, uint32_t boot_count, bool have_boot)
{
    if (!have_boot || (s->have_boot && boot_count != s->boot_count)) {
        s->have_mark = false;
    }
    s->boot_count = boot_count;
    s->have_boot = have_boot;
}

/* Filter out reconnect backlog records already seen in the same boot session. */
static bool session_record_new(session_state_t *s, const linsight_can_record_t *rec)
{
    if (!s->have_mark) {
        s->have_mark = true;
        s->last_ts_ms = rec->ts_ms;
        s->last_seq = rec->seq;
        return true;
    }
    if (rec->ts_ms < s->last_ts_ms) {
        return false;
    }
    if (rec->ts_ms == s->last_ts_ms) {
        const int16_t diff = (int16_t)(rec->seq - s->last_seq);
        if (diff <= 0 && diff > -1024) {
            return false;
        }
    }
    s->last_ts_ms = rec->ts_ms;
    s->last_seq = rec->seq;
    return true;
}

/* ------------------------------------------------------------ daemon state */

typedef struct {
    int fd;
    linsight_parser_t parser;
    uint8_t payload[LINSIGHT_MAX_CAN_BATCH];
    uint64_t recs;
    time_t last_rx;
    bool anchor_valid;
    session_state_t sess;
    uint8_t out[DEV_OUT_CAP];
    size_t out_off;
    size_t out_len;
} device_t;

static device_t g_dev = {.fd = -1};
static int g_epoll = -1;

/* ----------------------------------------------------- device TX & framing */

static void device_update_epoll(void)
{
    if (g_dev.fd < 0) {
        return;
    }
    struct epoll_event ev = {
        .events = EPOLLIN | (g_dev.out_len > g_dev.out_off ? EPOLLOUT : 0),
        .data.u64 = EPOLL_TAG_DEVICE,
    };
    epoll_ctl(g_epoll, EPOLL_CTL_MOD, g_dev.fd, &ev);
}

static void device_queue_pong(const void *payload, uint16_t len)
{
    if (g_dev.fd < 0 || len > LINSIGHT_MAX_PONG) {
        return;
    }
    if (g_dev.out_off > 0) {
        memmove(g_dev.out, g_dev.out + g_dev.out_off, g_dev.out_len - g_dev.out_off);
        g_dev.out_len -= g_dev.out_off;
        g_dev.out_off = 0;
    }
    const size_t total = LINSIGHT_FRAME_HEADER_SIZE + len;
    if (total > sizeof(g_dev.out) - g_dev.out_len) {
        return;
    }
    linsight_frame_write_header(g_dev.out + g_dev.out_len, LINSIGHT_FRAME_PONG, len);
    if (len) {
        memcpy(g_dev.out + g_dev.out_len + LINSIGHT_FRAME_HEADER_SIZE, payload, len);
    }
    g_dev.out_len += total;
    device_update_epoll();
}

static void device_flush(void)
{
    while (g_dev.fd >= 0 && g_dev.out_off < g_dev.out_len) {
        const ssize_t n = send(g_dev.fd, g_dev.out + g_dev.out_off,
                               g_dev.out_len - g_dev.out_off, MSG_NOSIGNAL);
        if (n > 0) {
            g_dev.out_off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        device_close();
        return;
    }
    if (g_dev.out_off == g_dev.out_len) {
        g_dev.out_off = 0;
        g_dev.out_len = 0;
    }
    device_update_epoll();
}

static void handle_batch(const uint8_t *payload, uint16_t len)
{
    if (g_dev.fd < 0 || len < 2) {
        return;
    }
    const uint16_t count = linsight_get_u16(payload);
    const uint32_t need = 2u + (uint32_t)count * LINSIGHT_REC_SIZE;
    if (count == 0 || count > LINSIGHT_BATCH_MAX_RECORDS || (uint32_t)len < need) {
        fprintf(stderr, TAG ": bad batch len %u count %u\n", (unsigned)len,
                (unsigned)count);
        return;
    }

    static linsight_can_record_t recs[LINSIGHT_BATCH_MAX_RECORDS];
    uint32_t valid = 0;
    uint32_t accepted = 0;
    for (uint16_t i = 0; i < count; i++) {
        linsight_can_record_t r;
        if (linsight_record_unpack(payload + 2 + (size_t)i * LINSIGHT_REC_SIZE, &r)) {
            valid++;
            if (session_record_new(&g_dev.sess, &r)) {
                recs[accepted++] = r;
            }
        }
    }
    g_dev.recs += valid;
    if (!accepted || !vcan_is_open()) {
        return;
    }

    const uint64_t now = mono_ns();
    if (!g_dev.anchor_valid) {
        pacer_anchor(now, recs[0].ts_ms);
        g_dev.anchor_valid = true;
    }
    pacer_submit(recs, accepted, now);
}

static void handle_frame(void *ctx, uint8_t type, const uint8_t *payload, uint16_t len)
{
    (void)ctx;
    switch (type) {
    case LINSIGHT_FRAME_HELLO: {
        fprintf(stderr, TAG ": device hello: %.*s\n", (int)len, (const char *)payload);
        if (!payload || len == 0 || len > LINSIGHT_MAX_HELLO) {
            break;
        }
        char json[LINSIGHT_MAX_HELLO + 1];
        memcpy(json, payload, len);
        json[len] = '\0';

        uint32_t boot = 0;
        const bool has_boot = json_u32(json, "boot", &boot);
        session_reset(&g_dev.sess, boot, has_boot);

        uint32_t uptime_ms = 0;
        if (json_u32(json, "uptime_ms", &uptime_ms)) {
            pacer_anchor(mono_ns(), uptime_ms);
            g_dev.anchor_valid = true;
        }
        break;
    }
    case LINSIGHT_FRAME_CAN_BATCH:
        handle_batch(payload, len);
        break;
    case LINSIGHT_FRAME_EVENT:
        fprintf(stderr, TAG ": device event: %.*s\n", (int)len, (const char *)payload);
        break;
    case LINSIGHT_FRAME_PING:
        device_queue_pong(payload, len);
        break;
    default:
        break;
    }
}

/* ----------------------------------------------------- connection & events */

void device_init(int epoll_fd)
{
    g_epoll = epoll_fd;
    linsight_parser_init(&g_dev.parser, g_dev.payload, LINSIGHT_MAX_CAN_BATCH);
}

void device_close(void)
{
    if (g_dev.fd < 0) {
        return;
    }
    epoll_ctl(g_epoll, EPOLL_CTL_DEL, g_dev.fd, NULL);
    close(g_dev.fd);
    g_dev.fd = -1;
    g_dev.out_off = 0;
    g_dev.out_len = 0;
    g_dev.anchor_valid = false;
    fprintf(stderr, TAG ": device session ended (%llu records)\n",
            (unsigned long long)g_dev.recs);
}

void device_accept(int listen_fd)
{
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    const int fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
    if (fd < 0) {
        return;
    }
    if (g_dev.fd >= 0) {
        fprintf(stderr, TAG ": replacing existing device session\n");
        device_close();
    }

    set_nonblock(fd);
    static const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    g_dev.fd = fd;
    g_dev.recs = 0;
    g_dev.last_rx = time(NULL);
    g_dev.anchor_valid = false;
    linsight_parser_reset(&g_dev.parser);

    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = EPOLL_TAG_DEVICE};
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, fd, &ev);

    char ip[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip))) {
        strcpy(ip, "?");
    }
    fprintf(stderr, TAG ": device connected from %s:%u\n", ip,
            (unsigned)ntohs(peer.sin_port));
}

void device_event(uint32_t revents)
{
    if (revents & EPOLLOUT) {
        device_flush();
    }
    if (g_dev.fd < 0 || !(revents & (EPOLLIN | EPOLLERR | EPOLLHUP))) {
        return;
    }
    uint8_t rx[RX_BUF_SIZE];
    const ssize_t got = recv(g_dev.fd, rx, sizeof(rx), 0);
    if (got <= 0) {
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        device_close();
        return;
    }
    g_dev.last_rx = time(NULL);
    if (linsight_parser_feed(&g_dev.parser, rx, (size_t)got, handle_frame, NULL) !=
        LINSIGHT_PARSE_OK) {
        fprintf(stderr, TAG ": protocol error, closing device\n");
        device_close();
    }
}

void device_check_idle(void)
{
    if (g_dev.fd >= 0 && g_dev.last_rx &&
        time(NULL) - g_dev.last_rx > IDLE_TIMEOUT_SEC) {
        fprintf(stderr, TAG ": device idle > %ds, closing\n", IDLE_TIMEOUT_SEC);
        device_close();
    }
}

uint64_t device_records(void)
{
    return g_dev.recs;
}
