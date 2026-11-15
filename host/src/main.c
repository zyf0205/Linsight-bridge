/*
 * linsightd — LinSight CAN bridge daemon (M3).
 *
 * Single-threaded epoll loop: TCP listener for the device, signalfd
 * (SIGTERM/SIGINT graceful exit) and a 1 Hz timerfd (idle watchdog + stats).
 * One device session at a time; CAN_BATCH records are de-duplicated against
 * reconnect backlogs, written as candump text (--dump) and paced onto a
 * SocketCAN interface (--vcan) by their device timestamps, so cycle times in
 * SavvyCAN/PlotJuggler are real. The bridge does not record: the device SD
 * black box (tools/lsc.py) or tcpdump/SavvyCAN own capture.
 */

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
#include "pace.h"
#include "session.h"

#define TAG "linsightd"
#define LINSIGHT_HOST_VERSION "0.6.0"

#define RX_BUF_SIZE 4096
#define IDLE_TIMEOUT_MS 30000u
#define DEV_OUT_CAP 4096u
#define MAX_EVENTS 64
#define PACE_PUMP_MAX 4096u           /* records forwarded per main-loop pump */
#define PACE_PUMP_BUDGET_NS 5000000ull /* ...or 5 ms of pump time, first wins */

static const char *g_port = "8888";
static const char *g_vcan_ifname;
static int g_vcan_fd = -1;
static uint64_t g_vcan_written;
static uint64_t g_vcan_dropped;
static const char *g_dump_path; /* "-" = stdout */
static FILE *g_dump;
static uint64_t g_dump_lines;
static double g_pace_speed = 1.0; /* 0 = burst (old unpaced behaviour) */
static bool g_pace_replay;        /* --pace-replay: 1:1 historical pacing */
static pace_t g_pace;

/* ------------------------------------------------------------- session */

typedef struct {
    int fd;
    linsight_parser_t parser;
    uint8_t payload[LINSIGHT_MAX_CAN_BATCH];
    uint64_t recs; /* raw records received this session */
    time_t last_rx;
    bool anchor_valid;
    uint64_t anchor_wall_ms;
    char device_id[16];
    uint32_t boot_count;
    session_state_t sess;

    /* Epoll-owned output queue: callbacks only append; EPOLLOUT drains it. */
    uint8_t out[DEV_OUT_CAP];
    size_t out_off;
    size_t out_len;
} device_t;

static device_t g_dev = {.fd = -1};
static int g_epoll = -1;
static volatile sig_atomic_t g_stop;

static void device_close(void);

static void set_nonblock(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t wall_ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
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

    struct sockaddr_can addr = {0};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, TAG ": vcan bind %s: %s\n", ifname, strerror(errno));
        close(fd);
        return -1;
    }

    set_nonblock(fd);
    return fd;
}

/* Best-effort, nonblocking: a full interface queue drops this frame and never
 * stalls the device RX path. */
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

        if (write(g_vcan_fd, &cf, sizeof(cf)) != (ssize_t)sizeof(cf)) {
            g_vcan_dropped++;
        } else {
            g_vcan_written++;
        }
    }
}

/* ------------------------------------------------------------ dump output */

/* Byte-compatible with the former canlog_format_candump_ms() so `--dump`
 * lines match `lsc.py export candump` (and can-utils candump) exactly. */
static int dump_format_line(const linsight_can_record_t *r, uint64_t start_wall_ms,
                            char *buf, size_t cap)
{
    const uint64_t wall_ms = start_wall_ms + (uint64_t)r->ts_ms;
    const uint64_t sec = wall_ms / 1000u;
    const uint64_t usec = (wall_ms % 1000u) * 1000u;
    const unsigned id = (unsigned)(r->id & 0x1FFFFFFFu);
    const bool ext = (r->flags & LINSIGHT_REC_FLAG_EXT) != 0;
    int used;
    if (ext) {
        used = snprintf(buf, cap, "(%llu.%06llu) can0 %08X#", (unsigned long long)sec,
                        (unsigned long long)usec, id);
    } else {
        used = snprintf(buf, cap, "(%llu.%06llu) can0 %03X#", (unsigned long long)sec,
                        (unsigned long long)usec, id);
    }
    if (used < 0 || (size_t)used >= cap) {
        return -1;
    }
    if (r->flags & LINSIGHT_REC_FLAG_RTR) {
        if ((size_t)used + 2 > cap) {
            return -1;
        }
        buf[used++] = 'R';
        buf[used] = '\0';
        return used;
    }
    for (uint32_t i = 0; i < r->dlc && i < LINSIGHT_DLC_MAX; i++) {
        if ((size_t)used + 3 > cap) {
            return -1;
        }
        const int n = snprintf(buf + used, cap - (size_t)used, "%02X", r->data[i]);
        if (n != 2) {
            return -1;
        }
        used += n;
    }
    return used;
}

static void dump_write_records(const linsight_can_record_t *recs, uint32_t n)
{
    if (!g_dump) {
        return;
    }
    char line[128];
    for (uint32_t i = 0; i < n; i++) {
        const int len = dump_format_line(&recs[i], g_dev.anchor_wall_ms, line,
                                         sizeof(line) - 1);
        if (len <= 0) {
            continue;
        }
        line[len] = '\n';
        if (fwrite(line, 1, (size_t)len + 1, g_dump) == (size_t)len + 1) {
            g_dump_lines++;
        }
    }
    if (n) {
        /* Per-batch flush: --dump behaves like a live stream (candump-like),
         * and tests can read the file without waiting for the 1 Hz tick. */
        fflush(g_dump);
    }
}

/* ------------------------------------------------------- output queues */

static void device_update_epoll(void)
{
    if (g_dev.fd < 0) {
        return;
    }
    struct epoll_event ev = {
        .events = EPOLLIN | (g_dev.out_len > g_dev.out_off ? EPOLLOUT : 0),
        .data.u64 = 0,
    };
    epoll_ctl(g_epoll, EPOLL_CTL_MOD, g_dev.fd, &ev);
}

/* Append one complete device frame. PONG is tiny; if the queue is full the
 * frame is dropped rather than blocking the epoll loop. */
static bool device_queue_frame(uint8_t type, const void *payload, uint16_t len)
{
    if (g_dev.fd < 0 || len > LINSIGHT_MAX_CONTROL) {
        return false;
    }
    if (g_dev.out_off > 0) {
        memmove(g_dev.out, g_dev.out + g_dev.out_off, g_dev.out_len - g_dev.out_off);
        g_dev.out_len -= g_dev.out_off;
        g_dev.out_off = 0;
    }
    const size_t total = LINSIGHT_FRAME_HEADER_SIZE + len;
    if (total > sizeof(g_dev.out) - g_dev.out_len) {
        return false;
    }
    linsight_frame_write_header(g_dev.out + g_dev.out_len, type, len);
    if (len) {
        memcpy(g_dev.out + g_dev.out_len + LINSIGHT_FRAME_HEADER_SIZE, payload, len);
    }
    g_dev.out_len += total;
    device_update_epoll();
    return true;
}

static void device_flush(void)
{
    if (g_dev.fd < 0) {
        return;
    }
    while (g_dev.out_off < g_dev.out_len) {
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

/* ------------------------------------------------------------- frames */

static void pace_bind_if_wall(void)
{
    if (g_vcan_fd >= 0 && g_pace_speed > 0.0 && !g_pace_replay) {
        pace_bind_wall(&g_pace, g_dev.anchor_wall_ms, mono_ns(), wall_ms_now());
    }
}

static void handle_batch(const uint8_t *payload, uint16_t len)
{
    if (g_dev.fd < 0 || len < 2) {
        return;
    }
    const uint16_t count = linsight_get_u16(payload);
    /* 32-bit length math: a u16 cast here would wrap for count > 3276 and
     * let a malformed frame read/write far past the buffers. */
    const uint32_t need = 2u + (uint32_t)count * LINSIGHT_REC_SIZE;
    if (count == 0 || count > LINSIGHT_BATCH_MAX_RECORDS || (uint32_t)len < need) {
        fprintf(stderr, TAG ": bad batch len %u count %u\n", (unsigned)len,
                (unsigned)count);
        return;
    }

    static linsight_can_record_t recs[LINSIGHT_BATCH_MAX_RECORDS];
    uint32_t valid = 0;
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *p = payload + 2 + (size_t)i * LINSIGHT_REC_SIZE;
        linsight_can_record_t r;
        if (linsight_record_unpack(p, &r)) {
            recs[valid++] = r;
        }
    }
    if (valid < count) {
        fprintf(stderr, TAG ": batch had %u invalid records\n",
                (unsigned)(count - valid));
    }

    /* Dedupe against a reconnect backlog and count gaps; only accepted
     * records reach the dump and the vcan forwarder. */
    uint32_t accepted = 0;
    for (uint32_t i = 0; i < valid; i++) {
        if (session_record_new(&g_dev.sess, &recs[i])) {
            recs[accepted++] = recs[i];
        }
    }
    g_dev.recs += valid;
    if (!accepted) {
        return;
    }

    if (!g_dev.anchor_valid) {
        /* Old firmware without HELLO uptime: map the first record's
         * uptime onto the current wall clock. */
        g_dev.anchor_wall_ms =
            session_anchor_start_wall_ms(wall_ms_now(), recs[0].ts_ms);
        g_dev.anchor_valid = true;
        pace_bind_if_wall();
    }
    dump_write_records(recs, accepted);
    if (g_vcan_fd < 0 || g_pace_speed <= 0.0) {
        vcan_forward(recs, accepted);
        return;
    }
    const uint64_t now = mono_ns();
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
        session_hello_t h;
        if (!session_parse_hello(payload, len, &h)) {
            break;
        }
        if (h.has_device_id) {
            snprintf(g_dev.device_id, sizeof(g_dev.device_id), "%s", h.device_id);
        }
        g_dev.boot_count = h.boot_count;
        session_reset(&g_dev.sess, h.boot_count, h.has_boot);
        if (h.has_uptime) {
            g_dev.anchor_wall_ms =
                session_anchor_start_wall_ms(wall_ms_now(), h.uptime_ms);
            g_dev.anchor_valid = true;
            pace_bind_if_wall();
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
        (void)device_queue_frame(LINSIGHT_FRAME_PONG, payload, len);
        break;
    default:
        /* STATS/DEVICE_INFO/PONG/ACK are ignored: the bridge has no CLI. */
        break;
    }
}

/* ------------------------------------------------------------- sessions */

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
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) {
        return;
    }
    if (g_dev.fd >= 0) {
        fprintf(stderr, TAG ": replacing existing device session\n");
        device_close();
    }

    set_nonblock(fd);
    static int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    g_dev.fd = fd;
    g_dev.recs = 0;
    g_dev.last_rx = time(NULL);
    g_dev.anchor_valid = false;
    session_clear_counters(&g_dev.sess);
    linsight_parser_reset(&g_dev.parser);
    /* The pace queue survives the reconnect: backlog records already queued
     * keep their targets; --pace-replay re-bases onto "now". */
    pace_new_session(&g_pace, mono_ns());

    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 0}; /* u64 0 = device */
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, fd, &ev);

    fprintf(stderr, TAG ": device connected\n");
}

/* ---------------------------------------------------------- pacing pump */

/* Forward due records within a per-iteration budget (records / time, first
 * wins). Leftovers stay queued; pace_epoll_timeout_ms() then returns 0 and
 * the next epoll iteration continues immediately, so a burst never delays
 * PONG/event handling by more than one budget window. */
static void pace_pump(void)
{
    if (g_vcan_fd < 0 || g_pace_speed <= 0.0) {
        return;
    }
    static linsight_can_record_t out[256];
    const uint64_t t0 = mono_ns();
    uint32_t budget = PACE_PUMP_MAX;
    while (budget) {
        const uint32_t want = budget < (uint32_t)(sizeof(out) / sizeof(out[0]))
                                  ? budget
                                  : (uint32_t)(sizeof(out) / sizeof(out[0]));
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

/* ------------------------------------------------------------- listener */

static int make_tcp_listener(const char *port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    static const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)};
    addr.sin_port = htons((uint16_t)atoi(port));
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    return fd;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            const int p = atoi(argv[++i]);
            if (p <= 0 || p > 65535) {
                fprintf(stderr, "linsightd: bad port '%s'\n", argv[i]);
                return 1;
            }
            g_port = argv[i];
        } else if (strcmp(argv[i], "--vcan") == 0 && i + 1 < argc) {
            g_vcan_ifname = argv[++i];
        } else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            g_dump_path = argv[++i];
        } else if (strcmp(argv[i], "--pace") == 0 && i + 1 < argc) {
            char *end = NULL;
            g_pace_speed = strtod(argv[++i], &end);
            if (end == argv[i] || *end || g_pace_speed < 0.0) {
                fprintf(stderr, "linsightd: bad pace speed '%s'\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--pace-replay") == 0) {
            g_pace_replay = true;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("linsightd %s\n", LINSIGHT_HOST_VERSION);
            return 0;
        } else {
            fprintf(stderr,
                    "usage: linsightd [--port N] [--vcan IFACE] [--dump PATH|-]\n"
                    "                  [--pace SPEED] [--pace-replay] [--version]\n"
                    "  --dump        candump text, device-timestamped ('-' = stdout)\n"
                    "  --pace SPEED  vcan pacing: 0 = burst, >0 wall 1:1 (default 1.0);\n"
                    "                SPEED scales intervals only with --pace-replay\n"
                    "  --pace-replay replay historical gaps 1:1 instead of live wall time\n");
            return 1;
        }
    }

    if (g_dump_path) {
        if (strcmp(g_dump_path, "-") == 0) {
            g_dump = stdout;
        } else {
            g_dump = fopen(g_dump_path, "w");
            if (!g_dump) {
                fprintf(stderr, TAG ": dump %s: %s\n", g_dump_path, strerror(errno));
                return 1;
            }
        }
    }

    if (g_vcan_ifname) {
        g_vcan_fd = vcan_open(g_vcan_ifname);
        if (g_vcan_fd < 0) {
            return 1;
        }
    }
    /* Always initialize the pacer: pace_count()/pace_new_session() run even
     * without --vcan (stats line, session bookkeeping). */
    pace_reset(&g_pace, g_pace_replay ? PACE_ANCHOR_FIRST : PACE_ANCHOR_WALL,
               g_pace_speed);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGPIPE);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    /* Nonblocking: the drain loops below must never block the main loop. */
    const int sigfd = signalfd(-1, &mask, SFD_NONBLOCK);

    const int tcp_fd = make_tcp_listener(g_port);
    if (tcp_fd < 0 || sigfd < 0) {
        fprintf(stderr, TAG ": listener setup failed: %s\n", strerror(errno));
        return 1;
    }

    g_epoll = epoll_create1(0);
    linsight_parser_init(&g_dev.parser, g_dev.payload, LINSIGHT_MAX_CAN_BATCH);
    if (g_epoll < 0) {
        fprintf(stderr, TAG ": epoll_create1 failed: %s\n", strerror(errno));
        return 1;
    }

    struct epoll_event ev = {0}; /* zero-init: u64 tags must not carry stack garbage */
    ev.events = EPOLLIN;
    ev.data.u64 = 1; /* tcp listener */
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, tcp_fd, &ev);
    ev.data.u64 = 3; /* signalfd */
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, sigfd, &ev);

    const int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (tfd < 0) {
        fprintf(stderr, TAG ": timerfd_create failed: %s\n", strerror(errno));
        return 1;
    }
    struct itimerspec its = {
        .it_interval = {.tv_sec = 1},
        .it_value = {.tv_sec = 1},
    };
    timerfd_settime(tfd, 0, &its, NULL);
    ev.data.u64 = 4; /* timer */
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, tfd, &ev);

    fprintf(stderr, TAG ": listening on :%s", g_port);
    if (g_vcan_fd >= 0) {
        fprintf(stderr, ", forwarding to SocketCAN %s (%s)", g_vcan_ifname,
                g_pace_speed > 0.0
                    ? (g_pace_replay ? "paced replay" : "paced wall 1:1")
                    : "burst");
    }
    if (g_dump) {
        fprintf(stderr, ", dumping candump to %s",
                g_dump == stdout ? "stdout" : g_dump_path);
    }
    fprintf(stderr, "\n");

    uint64_t last_recs = 0;
    while (!g_stop) {
        const int timeout =
            (g_vcan_fd >= 0 && g_pace_speed > 0.0) ? pace_epoll_timeout_ms(&g_pace, mono_ns())
                                                   : -1;
        struct epoll_event events[MAX_EVENTS];
        const int n = epoll_wait(g_epoll, events, MAX_EVENTS, timeout);
        for (int i = 0; i < n; i++) {
            const uint64_t tag = events[i].data.u64;

            if (tag == 3) { /* signal */
                struct signalfd_siginfo si;
                while (read(sigfd, &si, sizeof(si)) == sizeof(si)) {
                    g_stop = 1;
                }
            } else if (tag == 4) { /* 1 Hz stats */
                uint64_t misses;
                while (read(tfd, &misses, sizeof(misses)) == sizeof(misses)) {
                }
                /* Idle watchdog: the device pings every 5 s; a silent session
                 * for IDLE_TIMEOUT_MS is a dead link (e.g. lost Wi-Fi). */
                if (g_dev.fd >= 0 && g_dev.last_rx &&
                    time(NULL) - g_dev.last_rx > IDLE_TIMEOUT_MS / 1000u) {
                    fprintf(stderr, TAG ": device idle > %us, closing\n",
                            (unsigned)(IDLE_TIMEOUT_MS / 1000u));
                    device_close();
                }
                if (g_dump) {
                    fflush(g_dump);
                }
                /* A reconnect resets g_dev.recs; report just the new session
                 * instead of underflowing the delta. */
                const uint64_t delta =
                    g_dev.recs >= last_recs ? g_dev.recs - last_recs : g_dev.recs;
                fprintf(stderr,
                        TAG ": recs=%llu (+%llu) dump=%llu vcan=%llu vcan_dropped=%llu paceq=%u\n",
                        (unsigned long long)g_dev.recs, (unsigned long long)delta,
                        (unsigned long long)g_dump_lines,
                        (unsigned long long)g_vcan_written,
                        (unsigned long long)g_vcan_dropped,
                        (unsigned)pace_count(&g_pace));
                last_recs = g_dev.recs;
            } else if (tag == 1) {
                device_accept(tcp_fd);
            } else if (tag == 0) { /* device socket */
                const uint32_t revents = events[i].events;
                if (revents & EPOLLOUT) {
                    device_flush();
                }
                if (g_dev.fd < 0) {
                    continue;
                }
                if (!(revents & (EPOLLIN | EPOLLERR | EPOLLHUP))) {
                    continue;
                }
                uint8_t rx[RX_BUF_SIZE];
                const ssize_t got = recv(g_dev.fd, rx, sizeof(rx), 0);
                if (got <= 0) {
                    if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        continue;
                    }
                    device_close();
                    continue;
                }
                g_dev.last_rx = time(NULL);
                if (linsight_parser_feed(&g_dev.parser, rx, (size_t)got, handle_frame,
                                         NULL) != LINSIGHT_PARSE_OK) {
                    fprintf(stderr, TAG ": protocol error, closing device\n");
                    device_close();
                }
            }
        }
        pace_pump();
    }

    device_close();
    if (g_vcan_fd >= 0 && g_pace_speed > 0.0) {
        /* Drain whatever is queued: targets are in the past by now, burst. */
        static linsight_can_record_t out[256];
        uint32_t got;
        while ((got = pace_pop_due(&g_pace, UINT64_MAX, out,
                                  (uint32_t)(sizeof(out) / sizeof(out[0])))) > 0) {
            vcan_forward(out, got);
        }
    }
    if (g_dump) {
        fflush(g_dump);
        if (g_dump != stdout) {
            fclose(g_dump);
        }
    }
    if (g_vcan_fd >= 0) {
        close(g_vcan_fd);
        g_vcan_fd = -1;
    }
    close(tcp_fd);
    fprintf(stderr, TAG ": exited cleanly\n");
    return 0;
}
