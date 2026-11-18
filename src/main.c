/*
 * linsightd — LinSight CAN to SocketCAN bridge daemon.
 *
 * Single-threaded Linux epoll loop:
 *   - TCP listener (:8888) + single active nonblocking device session
 *   - UDP discovery responder (:8889, "LINSIGHT?1" -> "LINSIGHT!1 <port>\n")
 *   - signalfd (SIGINT/SIGTERM graceful exit, SIGPIPE blocked)
 *   - 1 Hz timerfd (30 s idle watchdog + stderr stats)
 *   - PF_CAN (SOCK_RAW) SocketCAN forwarding with CLOCK_MONOTONIC 1:1 pacing
 */

#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "frame.h"

#define TAG "linsightd"
#define LINSIGHT_HOST_VERSION "0.7.0"

#define DISCOVERY_PORT_DEFAULT 8889
#define DISCOVERY_PROBE "LINSIGHT?1"
#define DISCOVERY_PROBE_LEN 10u

#define RX_BUF_SIZE 4096
#define IDLE_TIMEOUT_SEC 30
#define DEV_OUT_CAP 64u
#define MAX_EVENTS 16

#define PACEQ_CAP 262144u              /* 262k-entry drop-oldest ring (~8 MiB) */
#define PACE_NS_PER_MS 1000000ULL      /* 1 ms in nanoseconds */
#define PACE_PUMP_MAX 4096u            /* max frames per epoll iteration */
#define PACE_PUMP_BUDGET_NS 5000000ULL /* max 5 ms pump time per iteration */

enum {
    EPOLL_TAG_DEVICE = 0,
    EPOLL_TAG_TCP_LISTEN = 1,
    EPOLL_TAG_DISCOVERY = 2,
    EPOLL_TAG_SIGNAL = 3,
    EPOLL_TAG_TIMER = 4,
};

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

/* --------------------------------------------- monotonic wall-clock pacer */

typedef struct {
    linsight_can_record_t rec;
    uint64_t target_ns;
} pace_entry_t;

typedef struct {
    pace_entry_t q[PACEQ_CAP];
    uint32_t head;
    uint32_t count;
    bool anchored;
    uint64_t anchor_mono_ns;
    uint32_t anchor_dev_ms;
    bool have_last;
    uint32_t last_dev_ms;
    uint64_t last_target_ns;
} pace_t;

static void pace_anchor(pace_t *p, uint64_t now_ns, uint32_t dev_ms)
{
    p->anchor_mono_ns = now_ns;
    p->anchor_dev_ms = dev_ms;
    p->anchored = true;
    p->last_dev_ms = 0;
}

static void pace_push(pace_t *p, const linsight_can_record_t *r, uint64_t now_ns)
{
    uint64_t target = now_ns;
    if (p->have_last && r->ts_ms < p->last_dev_ms) {
        target = p->last_target_ns;
    } else if (p->anchored && r->ts_ms > p->anchor_dev_ms) {
        target = p->anchor_mono_ns +
                 (uint64_t)(r->ts_ms - p->anchor_dev_ms) * PACE_NS_PER_MS;
    } else if (p->anchored) {
        target = p->anchor_mono_ns;
    }

    if (p->have_last &&
        (target < p->last_target_ns || target - p->last_target_ns < PACE_NS_PER_MS)) {
        target = p->last_target_ns;
    }

    if (p->count == PACEQ_CAP) {
        p->head = (p->head + 1u) % PACEQ_CAP;
        p->count--;
    }
    p->q[(p->head + p->count) % PACEQ_CAP] =
        (pace_entry_t){.rec = *r, .target_ns = target};
    p->count++;
    p->have_last = true;
    p->last_dev_ms = r->ts_ms;
    p->last_target_ns = target;
}

static uint32_t pace_pop_due(pace_t *p, uint64_t now_ns, linsight_can_record_t *out,
                             uint32_t cap)
{
    uint32_t n = 0;
    while (n < cap && p->count > 0 && p->q[p->head].target_ns <= now_ns) {
        out[n++] = p->q[p->head].rec;
        p->head = (p->head + 1u) % PACEQ_CAP;
        p->count--;
    }
    return n;
}

static int pace_epoll_timeout_ms(const pace_t *p, uint64_t now_ns)
{
    if (p->count == 0) {
        return -1;
    }
    const uint64_t target = p->q[p->head].target_ns;
    if (target <= now_ns) {
        return 0;
    }
    const uint64_t diff = target - now_ns;
    const uint64_t ms = diff / PACE_NS_PER_MS + (diff % PACE_NS_PER_MS != 0 ? 1u : 0u);
    return ms > 0x7FFFFFFFull ? 0x7FFFFFFF : (int)ms;
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

static const char *g_port = "8888";
static int g_discovery_port = DISCOVERY_PORT_DEFAULT;
static int g_discovery_fd = -1;
static const char *g_vcan_ifname;
static int g_vcan_fd = -1;
static uint64_t g_vcan_written;
static uint64_t g_vcan_dropped;
static bool g_paced = true;
static pace_t g_pace;
static device_t g_dev = {.fd = -1};
static int g_epoll = -1;
static volatile sig_atomic_t g_stop;

static void device_close(void);

static void set_nonblock(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------- SocketCAN forward */

static int vcan_open(const char *ifname)
{
    const int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        fprintf(stderr, TAG ": vcan socket: %s\n", strerror(errno));
        return -1;
    }

    struct ifreq ifr = {0};
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, TAG ": vcan %s: %s\n", ifname, strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_can addr = {
        .can_family = AF_CAN,
        .can_ifindex = ifr.ifr_ifindex,
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, TAG ": vcan bind %s: %s\n", ifname, strerror(errno));
        close(fd);
        return -1;
    }

    set_nonblock(fd);
    return fd;
}

static void vcan_forward(const linsight_can_record_t *recs, uint32_t n)
{
    if (g_vcan_fd < 0) {
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        struct can_frame cf = {0};
        if (recs[i].flags & LINSIGHT_REC_FLAG_EXT) {
            cf.can_id = (recs[i].id & CAN_EFF_MASK) | CAN_EFF_FLAG;
        } else {
            cf.can_id = recs[i].id & CAN_SFF_MASK;
        }
        if (recs[i].flags & LINSIGHT_REC_FLAG_RTR) {
            cf.can_id |= CAN_RTR_FLAG;
        }
        if (recs[i].flags & LINSIGHT_REC_FLAG_ERR) {
            cf.can_id |= CAN_ERR_FLAG;
        }
        cf.can_dlc = recs[i].dlc > 8 ? 8 : recs[i].dlc;
        memcpy(cf.data, recs[i].data, cf.can_dlc);

        if (write(g_vcan_fd, &cf, sizeof(cf)) == (ssize_t)sizeof(cf)) {
            g_vcan_written++;
        } else {
            g_vcan_dropped++;
        }
    }
}

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
    if (!accepted || g_vcan_fd < 0) {
        return;
    }

    const uint64_t now = mono_ns();
    if (!g_dev.anchor_valid) {
        pace_anchor(&g_pace, now, recs[0].ts_ms);
        g_dev.anchor_valid = true;
    }
    if (!g_paced) {
        vcan_forward(recs, accepted);
        return;
    }
    for (uint32_t i = 0; i < accepted; i++) {
        pace_push(&g_pace, &recs[i], now);
    }
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
            pace_anchor(&g_pace, mono_ns(), uptime_ms);
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

/* ----------------------------------------------------- connection & pump */

static void device_close(void)
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

static void device_accept(int listen_fd)
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

static void pace_pump(void)
{
    if (g_vcan_fd < 0 || !g_paced) {
        return;
    }
    static linsight_can_record_t out[256];
    const uint64_t t0 = mono_ns();
    uint32_t budget = PACE_PUMP_MAX;
    while (budget) {
        const uint32_t want = budget < 256u ? budget : 256u;
        const uint32_t got = pace_pop_due(&g_pace, mono_ns(), out, want);
        if (!got) {
            break;
        }
        vcan_forward(out, got);
        budget -= got;
        if (mono_ns() - t0 > PACE_PUMP_BUDGET_NS) {
            break;
        }
    }
}

/* ------------------------------------------------ TCP listener & discovery */

static int make_tcp_listener(const char *port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    static const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons((uint16_t)atoi(port)),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    return fd;
}

static int make_discovery_socket(int port)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, TAG ": discovery socket: %s\n", strerror(errno));
        return -1;
    }
    set_nonblock(fd);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons((uint16_t)port),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, TAG ": discovery bind udp :%d: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void discovery_handle(int fd)
{
    uint8_t buf[64];
    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        const ssize_t n =
            recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &peer_len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                fprintf(stderr, TAG ": discovery recv: %s\n", strerror(errno));
            }
            return;
        }
        if (n != (ssize_t)DISCOVERY_PROBE_LEN ||
            memcmp(buf, DISCOVERY_PROBE, DISCOVERY_PROBE_LEN) != 0) {
            continue;
        }

        char reply[32];
        const int reply_len = snprintf(reply, sizeof(reply), "LINSIGHT!1 %s\n", g_port);
        if (reply_len > 0 && (size_t)reply_len < sizeof(reply)) {
            sendto(fd, reply, (size_t)reply_len, 0, (struct sockaddr *)&peer, peer_len);
        }
    }
}

/* -------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            const int p = atoi(argv[++i]);
            if (p <= 0 || p > 65535) {
                fprintf(stderr, TAG ": bad port '%s'\n", argv[i]);
                return 1;
            }
            g_port = argv[i];
        } else if (strcmp(argv[i], "--discovery-port") == 0 && i + 1 < argc) {
            char *end = NULL;
            const long p = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end || p < 0 || p > 65535) {
                fprintf(stderr, TAG ": bad discovery port '%s'\n", argv[i]);
                return 1;
            }
            g_discovery_port = (int)p;
        } else if (strcmp(argv[i], "--vcan") == 0 && i + 1 < argc) {
            g_vcan_ifname = argv[++i];
        } else if (strcmp(argv[i], "--no-pace") == 0) {
            g_paced = false;
        } else if (strcmp(argv[i], "--pace") == 0 && i + 1 < argc) {
            char *end = NULL;
            const double spd = strtod(argv[++i], &end);
            if (end == argv[i] || *end || spd < 0.0) {
                fprintf(stderr, TAG ": bad pace value '%s'\n", argv[i]);
                return 1;
            }
            g_paced = (spd > 0.0);
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("linsightd %s\n", LINSIGHT_HOST_VERSION);
            return 0;
        } else {
            fprintf(stderr,
                    "usage: linsightd [--port N] [--discovery-port N] [--vcan IFACE]\n"
                    "                 [--pace 0|1 | --no-pace] [--version]\n");
            return 1;
        }
    }

    if (g_vcan_ifname) {
        g_vcan_fd = vcan_open(g_vcan_ifname);
        if (g_vcan_fd < 0) {
            return 1;
        }
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGPIPE);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    const int sigfd = signalfd(-1, &mask, SFD_NONBLOCK);
    const int tcp_fd = make_tcp_listener(g_port);
    const int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    g_epoll = epoll_create1(0);

    if (tcp_fd < 0 || sigfd < 0 || tfd < 0 || g_epoll < 0) {
        fprintf(stderr, TAG ": initialization failed: %s\n", strerror(errno));
        return 1;
    }

    if (g_discovery_port != 0) {
        g_discovery_fd = make_discovery_socket(g_discovery_port);
    }

    linsight_parser_init(&g_dev.parser, g_dev.payload, LINSIGHT_MAX_CAN_BATCH);

    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = EPOLL_TAG_TCP_LISTEN};
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, tcp_fd, &ev);
    ev.data.u64 = EPOLL_TAG_SIGNAL;
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, sigfd, &ev);
    if (g_discovery_fd >= 0) {
        ev.data.u64 = EPOLL_TAG_DISCOVERY;
        epoll_ctl(g_epoll, EPOLL_CTL_ADD, g_discovery_fd, &ev);
    }

    struct itimerspec its = {
        .it_interval = {.tv_sec = 1},
        .it_value = {.tv_sec = 1},
    };
    timerfd_settime(tfd, 0, &its, NULL);
    ev.data.u64 = EPOLL_TAG_TIMER;
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, tfd, &ev);

    fprintf(stderr, TAG ": listening on :%s", g_port);
    if (g_discovery_fd >= 0) {
        fprintf(stderr, ", discovery udp :%d", g_discovery_port);
    }
    if (g_vcan_fd >= 0) {
        fprintf(stderr, ", SocketCAN %s (%s)", g_vcan_ifname,
                g_paced ? "paced 1:1" : "burst");
    }
    fprintf(stderr, "\n");

    uint64_t last_recs = 0;
    while (!g_stop) {
        const int timeout =
            (g_vcan_fd >= 0 && g_paced) ? pace_epoll_timeout_ms(&g_pace, mono_ns()) : -1;
        struct epoll_event events[MAX_EVENTS];
        const int n = epoll_wait(g_epoll, events, MAX_EVENTS, timeout);
        for (int i = 0; i < n; i++) {
            switch (events[i].data.u64) {
            case EPOLL_TAG_SIGNAL: {
                struct signalfd_siginfo si;
                while (read(sigfd, &si, sizeof(si)) == sizeof(si)) {
                    g_stop = 1;
                }
                break;
            }
            case EPOLL_TAG_TIMER: {
                uint64_t misses;
                while (read(tfd, &misses, sizeof(misses)) == sizeof(misses)) {
                }
                if (g_dev.fd >= 0 && g_dev.last_rx &&
                    time(NULL) - g_dev.last_rx > IDLE_TIMEOUT_SEC) {
                    fprintf(stderr, TAG ": device idle > %ds, closing\n", IDLE_TIMEOUT_SEC);
                    device_close();
                }
                const uint64_t delta =
                    g_dev.recs >= last_recs ? g_dev.recs - last_recs : g_dev.recs;
                fprintf(stderr,
                        TAG ": recs=%llu (+%llu) vcan=%llu vcan_dropped=%llu paceq=%u\n",
                        (unsigned long long)g_dev.recs, (unsigned long long)delta,
                        (unsigned long long)g_vcan_written,
                        (unsigned long long)g_vcan_dropped, (unsigned)g_pace.count);
                last_recs = g_dev.recs;
                break;
            }
            case EPOLL_TAG_DISCOVERY:
                discovery_handle(g_discovery_fd);
                break;
            case EPOLL_TAG_TCP_LISTEN:
                device_accept(tcp_fd);
                break;
            case EPOLL_TAG_DEVICE: {
                const uint32_t revents = events[i].events;
                if (revents & EPOLLOUT) {
                    device_flush();
                }
                if (g_dev.fd < 0 || !(revents & (EPOLLIN | EPOLLERR | EPOLLHUP))) {
                    break;
                }
                uint8_t rx[RX_BUF_SIZE];
                const ssize_t got = recv(g_dev.fd, rx, sizeof(rx), 0);
                if (got <= 0) {
                    if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    }
                    device_close();
                    break;
                }
                g_dev.last_rx = time(NULL);
                if (linsight_parser_feed(&g_dev.parser, rx, (size_t)got, handle_frame,
                                         NULL) != LINSIGHT_PARSE_OK) {
                    fprintf(stderr, TAG ": protocol error, closing device\n");
                    device_close();
                }
                break;
            }
            }
        }
        pace_pump();
    }

    device_close();
    if (g_vcan_fd >= 0 && g_paced) {
        static linsight_can_record_t out[256];
        uint32_t got;
        while ((got = pace_pop_due(&g_pace, UINT64_MAX, out, 256u)) > 0) {
            vcan_forward(out, got);
        }
    }
    if (g_vcan_fd >= 0) {
        close(g_vcan_fd);
    }
    if (g_discovery_fd >= 0) {
        close(g_discovery_fd);
    }
    close(tcp_fd);
    close(tfd);
    close(sigfd);
    close(g_epoll);
    fprintf(stderr, TAG ": exited cleanly\n");
    return 0;
}
