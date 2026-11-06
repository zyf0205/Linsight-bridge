/*
 * linsightd — LinSight CAN host daemon (M2).
 *
 * epoll main loop: TCP listener (the device), Unix-socket listener (the CLI),
 * an optional loopback TCP listener (Windows GUI / remote CLI), signalfd
 * (SIGTERM/SIGINT graceful exit) and a 1 Hz timerfd (stats).
 * One device session at a time; CAN_BATCH records flow into a bounded ring
 * drained by a writer thread that appends to per-session .lsc files.
 *
 * CLI commands (newline-terminated over the control socket):
 *   stats   -> one JSON status line, socket closes
 *   live    -> candump lines streamed until the client disconnects
 *   pause / resume / clear -> forwarded to the device as CONTROL, ACK echoed
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <time.h>
#include <unistd.h>

#include "canlog.h"
#include "frame.h"

#define TAG "linsightd"

#define WRING_CAP 16384u /* records; ~320 KB */
#define CLI_MAX 8
#define RX_BUF_SIZE 4096
#define IDLE_TIMEOUT_MS 30000u
#define LOG_BATCH 512u /* records per writer iteration */

static const char *g_port = "8888";
static const char *g_ctl_path = "/tmp/linsight.sock";
static const char *g_ctl_port = NULL; /* optional loopback TCP control port */
static const char *g_log_dir = ".";

/* ------------------------------------------------------------ write ring */

/* Every queued record carries the session generation it belongs to, so the
 * writer can order file close/open strictly against the record stream. A
 * device reconnect can otherwise queue old records behind a new OPEN request
 * and have them (or the first records of the new session) land in the wrong
 * .lsc file, which the old single-slot open/close flags could not prevent. */
typedef struct {
    linsight_can_record_t rec;
    uint32_t gen;
} wring_entry_t;

typedef struct {
    wring_entry_t entry[WRING_CAP];
    uint32_t head; /* next write index */
    uint32_t count;
    uint64_t dropped;
    uint64_t written;

    uint32_t gen; /* current device session generation */
    uint32_t bitrate; /* from the device HELLO, for the .lsc header */
    bool open_req;
    uint32_t open_gen;
    char open_path[512];
    bool close_req;
    uint32_t close_gen;

    /* Wall-clock origin for the session (device ts_ms is uptime), captured
     * when its first record arrives rather than when the writer drains it:
     * a backlogged writer must not shift recorded timestamps. */
    uint64_t anchor_wall;
    uint32_t anchor_gen;

    bool done;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} wring_t;

static wring_t g_ring = {
    .bitrate = 500000,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static void wring_push(const linsight_can_record_t *recs, uint32_t n)
{
    pthread_mutex_lock(&g_ring.lock);
    for (uint32_t i = 0; i < n; i++) {
        if (g_ring.count == WRING_CAP) {
            g_ring.head = (g_ring.head + 1) % WRING_CAP; /* drop oldest */
            g_ring.count--;
            g_ring.dropped++;
        }
        wring_entry_t *slot = &g_ring.entry[(g_ring.head + g_ring.count) % WRING_CAP];
        slot->rec = recs[i];
        slot->gen = g_ring.gen;
        g_ring.count++;
    }
    pthread_cond_signal(&g_ring.cond);
    pthread_mutex_unlock(&g_ring.lock);
}

/* Consume the pending open request only if it still belongs to `gen`: a newer
 * reconnect may have replaced it while the writer was busy. */
static bool wring_take_open(uint32_t gen, char *path, size_t cap, uint64_t *anchor)
{
    bool taken = false;
    pthread_mutex_lock(&g_ring.lock);
    if (g_ring.open_req && g_ring.open_gen == gen) {
        snprintf(path, cap, "%s", g_ring.open_path);
        *anchor = g_ring.anchor_gen == gen ? g_ring.anchor_wall : 0;
        g_ring.open_req = false;
        taken = true;
    }
    pthread_mutex_unlock(&g_ring.lock);
    return taken;
}

static void wring_set_anchor(uint64_t wall)
{
    pthread_mutex_lock(&g_ring.lock);
    g_ring.anchor_wall = wall;
    g_ring.anchor_gen = g_ring.gen;
    pthread_mutex_unlock(&g_ring.lock);
}

/* Close the log of `gen` if the device session is over. A close request for a
 * newer generation also ends `gen`: sessions only move forward, and the newer
 * close (e.g. a connect/disconnect that sent no records) must not leave the
 * old file open with a request the writer can never match, which would make it
 * spin on the condition variable. */
static bool wring_take_close(uint32_t gen)
{
    bool taken = false;
    pthread_mutex_lock(&g_ring.lock);
    if (g_ring.close_req && g_ring.close_gen >= gen) {
        g_ring.close_req = false;
        taken = true;
    }
    pthread_mutex_unlock(&g_ring.lock);
    return taken;
}

/* A close request older than the log we just opened refers to a session whose
 * open was superseded before it could create a file; there is nothing to
 * close, and leaving the flag set would make the writer spin forever. */
static void wring_clear_close_before(uint32_t gen)
{
    pthread_mutex_lock(&g_ring.lock);
    if (g_ring.close_req && g_ring.close_gen < gen) {
        g_ring.close_req = false;
    }
    pthread_mutex_unlock(&g_ring.lock);
}

/* With no log open there is nothing to close: any pending close belongs to a
 * generation that never produced a file (idle / immediate-reconnect session). */
static void wring_clear_close(void)
{
    pthread_mutex_lock(&g_ring.lock);
    g_ring.close_req = false;
    pthread_mutex_unlock(&g_ring.lock);
}

static uint32_t wring_bitrate(void)
{
    pthread_mutex_lock(&g_ring.lock);
    const uint32_t bitrate = g_ring.bitrate;
    pthread_mutex_unlock(&g_ring.lock);
    return bitrate;
}

static void wring_set_bitrate(uint32_t bitrate)
{
    pthread_mutex_lock(&g_ring.lock);
    g_ring.bitrate = bitrate;
    pthread_mutex_unlock(&g_ring.lock);
}

static void *writer_thread(void *arg)
{
    (void)arg;
    lsc_writer_t w = {0};
    bool log_open = false;
    uint32_t log_gen = 0;

    for (;;) {
        pthread_mutex_lock(&g_ring.lock);
        /* A pending open alone has no work: the file is created lazily from
         * the first record, so only records/close/shutdown wake the writer. */
        while (g_ring.count == 0 && !g_ring.close_req && !g_ring.done) {
            pthread_cond_wait(&g_ring.cond, &g_ring.lock);
        }
        const bool finish = g_ring.done;

        static wring_entry_t batch[LOG_BATCH];
        uint32_t n = g_ring.count;
        if (n > LOG_BATCH) {
            n = LOG_BATCH;
        }
        for (uint32_t i = 0; i < n; i++) {
            batch[i] = g_ring.entry[g_ring.head];
            g_ring.head = (g_ring.head + 1) % WRING_CAP;
        }
        g_ring.count -= n;
        pthread_mutex_unlock(&g_ring.lock);

        uint32_t ok = 0;
        for (uint32_t i = 0; i < n; i++) {
            const wring_entry_t *e = &batch[i];

            if (log_open && e->gen != log_gen) {
                /* Generation boundary: every record of the old session has
                 * been written, so it is safe to close its file now. */
                lsc_writer_close(&w);
                log_open = false;
                wring_take_close(log_gen);
            }
            if (!log_open) {
                char path[512];
                uint64_t start_wall = 0;
                if (wring_take_open(e->gen, path, sizeof(path), &start_wall)) {
                    if (!start_wall) {
                        /* No anchor recorded (should not happen): fall back to
                         * mapping this record's uptime onto the current time. */
                        const uint64_t now_s = (uint64_t)time(NULL);
                        const uint64_t ts_s = e->rec.ts_ms / 1000u;
                        start_wall = now_s > ts_s ? now_s - ts_s : now_s;
                    }
                    log_open = lsc_writer_open(&w, path, wring_bitrate(), start_wall) == 0;
                    log_gen = e->gen;
                    wring_clear_close_before(log_gen);
                    if (!log_open) {
                        fprintf(stderr, TAG ": cannot open %s: %s\n", path, strerror(errno));
                    }
                }
                if (!log_open) {
                    /* The open request was superseded (quick reconnect) or the
                     * file could not be created: drop this stale generation. */
                    continue;
                }
            }

            if (lsc_writer_write(&w, &e->rec) != 0) {
                fprintf(stderr, TAG ": log write failed: %s\n", strerror(errno));
                break;
            }
            ok++;
        }

        if (ok > 0) {
            pthread_mutex_lock(&g_ring.lock);
            g_ring.written += ok;
            pthread_mutex_unlock(&g_ring.lock);
        }

        /* The queue is drained (n < LOG_BATCH means it was fully popped).
         * Close a dead session; a close with no open log belongs to a session
         * that never produced a record, so there is no file to close. */
        if (n < LOG_BATCH) {
            if (log_open && wring_take_close(log_gen)) {
                lsc_writer_close(&w);
                log_open = false;
            }
            if (!log_open) {
                wring_clear_close();
            }
        }

        if (n > 0 && log_open) {
            lsc_writer_flush(&w);
        }

        if (finish) {
            pthread_mutex_lock(&g_ring.lock);
            /* A pending open without records has no file and needs no work. */
            const bool empty = g_ring.count == 0 && !g_ring.close_req;
            pthread_mutex_unlock(&g_ring.lock);
            if (empty) {
                if (log_open) {
                    lsc_writer_close(&w);
                }
                break;
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------- session */

typedef struct {
    int fd;
    linsight_parser_t parser;
    uint8_t payload[LINSIGHT_MAX_CAN_BATCH];
    uint64_t recs;
    char stats[512]; /* latest STATS JSON from the device */
    time_t last_rx;
    bool anchored; /* g_session_start mapped from the first record's uptime */
} device_t;

typedef struct {
    int fd;
    uint8_t buf[128];
    int len;
    bool live;
    bool filter_id_valid;
    uint32_t filter_id;
    int filter_ext; /* -1 = any, 0 = standard only, 1 = extended only */
    uint16_t pending_id; /* CONTROL id awaiting ACK, 0 = none */
} cli_conn_t;

static device_t g_dev = {.fd = -1};
static cli_conn_t g_cli[CLI_MAX];
static int g_epoll = -1;
static volatile sig_atomic_t g_stop;
static uint16_t g_ctrl_id;
static char g_current_log[512];
static uint64_t g_session_start;

static void set_nonblock(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static bool send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t left = len;
    while (left) {
        const ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Socket buffer full: wait briefly instead of tearing down the
                 * connection over a transient stall. */
                struct pollfd pfd = {.fd = fd, .events = POLLOUT};
                if (poll(&pfd, 1, 2000) <= 0) {
                    return false;
                }
                continue;
            }
            return false;
        }
        p += n;
        left -= (size_t)n;
    }
    return true;
}

static void send_frame(int fd, uint8_t type, const void *payload, uint16_t len)
{
    uint8_t hdr[LINSIGHT_FRAME_HEADER_SIZE];
    linsight_frame_write_header(hdr, type, len);
    if (!send_all(fd, hdr, sizeof(hdr)) ||
        (len && !send_all(fd, payload, len))) {
        fprintf(stderr, TAG ": send_frame type %u failed: %s\n", type, strerror(errno));
    }
}

static bool send_control_fields(int fd, const char *action, const char *fields)
{
    char json[LINSIGHT_MAX_CONTROL + 1];
    const bool has_fields = fields && fields[0];
    const int n = snprintf(json, sizeof(json), "{\"id\":%u,\"action\":\"%s\"%s%s}",
                           (unsigned)++g_ctrl_id, action, has_fields ? "," : "",
                           has_fields ? fields : "");
    if (n <= 0 || n > (int)LINSIGHT_MAX_CONTROL) {
        return false;
    }
    send_frame(fd, LINSIGHT_FRAME_CONTROL, json, (uint16_t)n);
    return true;
}

static bool send_control(int fd, const char *action)
{
    return send_control_fields(fd, action, NULL);
}

/* ------------------------------------------------------------- frames */

static void cli_close(cli_conn_t *c);

static void deliver_live(const linsight_can_record_t *recs, uint32_t n)
{
    char line[128];
    for (uint32_t i = 0; i < n; i++) {
        int len = 0;
        bool formatted = false;
        for (int c = 0; c < CLI_MAX; c++) {
            cli_conn_t *cli = &g_cli[c];
            if (cli->fd < 0 || !cli->live) {
                continue;
            }
            if (cli->filter_id_valid && recs[i].id != cli->filter_id) {
                continue;
            }
            const int rec_ext = (recs[i].flags & LINSIGHT_REC_FLAG_EXT) ? 1 : 0;
            if (cli->filter_ext >= 0 && rec_ext != cli->filter_ext) {
                continue;
            }
            if (!formatted) {
                /* Newline-terminated: `live` is a line-oriented stream for the
                 * CLI and the bench GUI. */
                len = canlog_format_candump(&recs[i], g_session_start, line,
                                            sizeof(line) - 1);
                formatted = true;
                if (len <= 0) {
                    break;
                }
                line[len++] = '\n';
            }
            /* Best-effort: a slow `live` consumer must never stall the device
             * RX / writer path. EAGAIN drops one line; a hard error or a
             * partial line (would corrupt the stream) closes the client. */
            const ssize_t sent = send(cli->fd, line, (size_t)len, MSG_NOSIGNAL);
            if (sent == len) {
                continue;
            }
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            cli_close(cli);
        }
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
        fprintf(stderr, TAG ": batch had %u invalid records\n", (unsigned)(count - valid));
    }

    g_dev.recs += valid;
    if (valid) {
        if (!g_dev.anchored) {
            /* Device ts_ms is uptime; anchor it to wall time once per session
             * so `live` lines and exports show real dates. */
            const uint64_t now_s = (uint64_t)time(NULL);
            const uint64_t ts_s = recs[0].ts_ms / 1000u;
            g_session_start = now_s > ts_s ? now_s - ts_s : now_s;
            wring_set_anchor(g_session_start);
            g_dev.anchored = true;
        }
        wring_push(recs, valid);
        deliver_live(recs, valid);
    }
}

/* HELLO is e.g. {"...","bitrate":"500k","mode":"LISTEN"}; the host has no
 * JSON dependency and this is the only field it needs from that object. */
static uint32_t parse_hello_bitrate(const uint8_t *payload, uint16_t len)
{
    char json[LINSIGHT_MAX_HELLO + 1];

    if (len > LINSIGHT_MAX_HELLO) {
        return 0;
    }
    memcpy(json, payload, len);
    json[len] = '\0';

    const char *key = strstr(json, "\"bitrate\":\"");
    if (!key) {
        return 0;
    }
    key += strlen("\"bitrate\":\"");
    if (strncmp(key, "125k", 4) == 0) {
        return 125000;
    }
    if (strncmp(key, "250k", 4) == 0) {
        return 250000;
    }
    if (strncmp(key, "500k", 4) == 0) {
        return 500000;
    }
    if (strncmp(key, "1M", 2) == 0) {
        return 1000000;
    }
    return 0;
}

static void handle_frame(void *ctx, uint8_t type, const uint8_t *payload, uint16_t len)
{
    (void)ctx;
    switch (type) {
    case LINSIGHT_FRAME_HELLO: {
        fprintf(stderr, TAG ": device hello: %.*s\n", (int)len, (const char *)payload);
        const uint32_t bitrate = parse_hello_bitrate(payload, len);
        if (bitrate) {
            wring_set_bitrate(bitrate);
        }
        break;
    }
    case LINSIGHT_FRAME_CAN_BATCH:
        handle_batch(payload, len);
        break;
    case LINSIGHT_FRAME_STATS:
        if (len < sizeof(g_dev.stats)) {
            memcpy(g_dev.stats, payload, len);
            g_dev.stats[len] = '\0';
        }
        break;
    case LINSIGHT_FRAME_EVENT:
        fprintf(stderr, TAG ": device event: %.*s\n", (int)len, (const char *)payload);
        break;
    case LINSIGHT_FRAME_PING:
        send_frame(g_dev.fd, LINSIGHT_FRAME_PONG, payload, len);
        break;
    case LINSIGHT_FRAME_PONG:
        fprintf(stderr, TAG ": device pong (%u bytes)\n", (unsigned)len);
        break;
    case LINSIGHT_FRAME_ACK: {
        char ack[64] = {0};
        memcpy(ack, payload, len < sizeof(ack) - 1 ? len : sizeof(ack) - 1);
        unsigned ack_id = 0;
        sscanf(ack, "{\"id\":%u", &ack_id);
        for (int c = 0; c < CLI_MAX; c++) {
            if (g_cli[c].fd >= 0 && g_cli[c].pending_id == (uint16_t)ack_id) {
                g_cli[c].pending_id = 0;
                send_all(g_cli[c].fd, ack, strlen(ack));
                send_all(g_cli[c].fd, "\n", 1);
                cli_close(&g_cli[c]); /* one reply, then EOF */
            }
        }
        break;
    }
    default:
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
    fprintf(stderr, TAG ": device session ended (%llu records)\n",
            (unsigned long long)g_dev.recs);
    pthread_mutex_lock(&g_ring.lock);
    g_ring.close_req = true;
    g_ring.close_gen = g_ring.gen;
    pthread_cond_signal(&g_ring.cond);
    pthread_mutex_unlock(&g_ring.lock);
    g_current_log[0] = '\0';
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
    g_dev.stats[0] = '\0';
    g_dev.anchored = false;
    linsight_parser_reset(&g_dev.parser);
    g_session_start = (uint64_t)time(NULL);

    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 0}; /* u64 0 = device */
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, fd, &ev);

    pthread_mutex_lock(&g_ring.lock);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    /* Monotonic suffix: two sessions in the same second must not truncate
     * each other's log. */
    static uint32_t session_seq;
    session_seq++;
    g_ring.gen++;
    g_ring.open_gen = g_ring.gen;
    snprintf(g_ring.open_path, sizeof(g_ring.open_path),
             "%s/can-%04d%02d%02d-%02d%02d%02d-%03u.lsc", g_log_dir, tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, session_seq);
    snprintf(g_current_log, sizeof(g_current_log), "%s", g_ring.open_path);
    g_ring.open_req = true;
    pthread_cond_signal(&g_ring.cond);
    pthread_mutex_unlock(&g_ring.lock);

    fprintf(stderr, TAG ": device connected, logging to %s\n", g_current_log);
}

static void cli_close(cli_conn_t *c)
{
    epoll_ctl(g_epoll, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    c->fd = -1;
    c->live = false;
    c->filter_id_valid = false;
    c->filter_ext = -1;
    c->pending_id = 0;
    c->len = 0;
}

/* "id=0x123 ext" / "std" tokens for a live subscription. */
static bool parse_live_filters(cli_conn_t *c, const char *args)
{
    c->filter_id_valid = false;
    c->filter_ext = -1;
    while (*args) {
        while (*args == ' ') {
            args++;
        }
        if (!*args) {
            break;
        }
        if (strncmp(args, "id=", 3) == 0) {
            char *end = NULL;
            const unsigned long v = strtoul(args + 3, &end, 0);
            if (end == args + 3 || v > 0x1FFFFFFFul) {
                return false;
            }
            c->filter_id_valid = true;
            c->filter_id = (uint32_t)v;
            args = end;
        } else if (strncmp(args, "ext", 3) == 0 &&
                   (args[3] == '\0' || args[3] == ' ')) {
            c->filter_ext = 1;
            args += 3;
        } else if (strncmp(args, "std", 3) == 0 &&
                   (args[3] == '\0' || args[3] == ' ')) {
            c->filter_ext = 0;
            args += 3;
        } else {
            return false;
        }
    }
    return true;
}

/* Build the optional JSON fragment for set_config from "bitrate=... mode=..."
 * tokens. Only whitelisted values reach the device; anything else is rejected
 * without forwarding a partial request. */
static bool build_config_fields(const char *args, char *out, size_t cap)
{
    size_t used = 0;
    bool any = false;
    while (*args) {
        while (*args == ' ') {
            args++;
        }
        if (!*args) {
            break;
        }
        if (strncmp(args, "bitrate=", 8) == 0) {
            char *end = NULL;
            const long v = strtol(args + 8, &end, 10);
            if (end == args + 8 ||
                (v != 125000 && v != 250000 && v != 500000 && v != 1000000)) {
                return false;
            }
            const int n = snprintf(out + used, cap - used, "%s\"bitrate\":%ld",
                                   any ? "," : "", v);
            if (n < 0 || (size_t)n >= cap - used) {
                return false;
            }
            used += (size_t)n;
            any = true;
            args = end;
        } else if (strncmp(args, "mode=", 5) == 0) {
            const char *start = args + 5;
            const char *end = start;
            while (*end && *end != ' ') {
                end++;
            }
            const size_t len = (size_t)(end - start);
            if (len == 0 || len >= 16) {
                return false;
            }
            char mode[16];
            memcpy(mode, start, len);
            mode[len] = '\0';
            if (strcasecmp(mode, "listen") != 0 &&
                strcasecmp(mode, "listen_only") != 0 &&
                strcasecmp(mode, "normal") != 0 &&
                strcasecmp(mode, "selftest") != 0 &&
                strcasecmp(mode, "no_ack") != 0) {
                return false;
            }
            const int n = snprintf(out + used, cap - used, "%s\"mode\":\"%s\"",
                                   any ? "," : "", mode);
            if (n < 0 || (size_t)n >= cap - used) {
                return false;
            }
            used += (size_t)n;
            any = true;
            args = end;
        } else {
            return false;
        }
    }
    return any;
}

static void cli_dispatch(cli_conn_t *c)
{
    char cmd[64] = {0};
    memcpy(cmd, c->buf, c->len < (int)sizeof(cmd) - 1 ? (size_t)c->len : sizeof(cmd) - 1);
    c->len = 0;
    for (char *p = cmd; *p; p++) {
        if (*p == '\n' || *p == '\r') {
            *p = '\0';
        }
    }

    if (strcmp(cmd, "stats") == 0) {
        char out[1024];
        pthread_mutex_lock(&g_ring.lock);
        const uint64_t written = g_ring.written;
        const uint64_t dropped = g_ring.dropped;
        pthread_mutex_unlock(&g_ring.lock);
        const int n = snprintf(
            out, sizeof(out),
            "{\"device\":%s,\"records\":%llu,\"written\":%llu,\"dropped\":%llu,"
            "\"log\":\"%s\",\"stats\":%s}\n",
            g_dev.fd >= 0 ? "true" : "false", (unsigned long long)g_dev.recs,
            (unsigned long long)written, (unsigned long long)dropped,
            g_current_log[0] ? g_current_log : "",
            g_dev.stats[0] ? g_dev.stats : "{}");
        size_t out_len =
            (n <= 0) ? 0 : ((size_t)n >= sizeof(out) ? sizeof(out) - 1 : (size_t)n);
        send_all(c->fd, out, out_len);
        cli_close(c);
    } else if (strcmp(cmd, "live") == 0 ||
               (strncmp(cmd, "live ", 5) == 0)) {
        if (!parse_live_filters(c, cmd + 4)) {
            send_all(c->fd, "ERR bad live filter\n", strlen("ERR bad live filter\n"));
            cli_close(c);
            return;
        }
        c->live = true; /* records stream until the client goes away */
    } else if (strncmp(cmd, "config", 6) == 0 &&
               (cmd[6] == '\0' || cmd[6] == ' ')) {
        char fields[96];
        if (!build_config_fields(cmd + 6, fields, sizeof(fields))) {
            send_all(c->fd, "ERR bad config\n", strlen("ERR bad config\n"));
            cli_close(c);
            return;
        }
        if (g_dev.fd < 0) {
            send_all(c->fd, "ERR no device\n", strlen("ERR no device\n"));
            cli_close(c);
            return;
        }
        if (!send_control_fields(g_dev.fd, LINSIGHT_CTRL_CONFIG, fields)) {
            send_all(c->fd, "ERR send failed\n", strlen("ERR send failed\n"));
            cli_close(c);
            return;
        }
        c->pending_id = g_ctrl_id;
    } else if (strcmp(cmd, "pause") == 0 || strcmp(cmd, "resume") == 0 ||
               strcmp(cmd, "clear") == 0) {
        if (g_dev.fd < 0) {
            send_all(c->fd, "ERR no device\n", strlen("ERR no device\n"));
            cli_close(c);
            return;
        }
        /* Device protocol names the action "clear_stats". */
        send_control(g_dev.fd, strcmp(cmd, "clear") == 0 ? "clear_stats" : cmd);
        c->pending_id = g_ctrl_id;
    } else {
        send_all(c->fd, "ERR unknown command\n", strlen("ERR unknown command\n"));
        cli_close(c);
    }
}

static void cli_accept(int listen_fd)
{
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) {
        return;
    }

    cli_conn_t *slot = NULL;
    for (int i = 0; i < CLI_MAX; i++) {
        if (g_cli[i].fd < 0) {
            slot = &g_cli[i];
            break;
        }
    }
    if (!slot) {
        close(fd);
        return;
    }

    set_nonblock(fd);
    memset(slot, 0, sizeof(*slot));
    slot->fd = fd;
    slot->filter_ext = -1;
    /* Use u64 tags everywhere; .ptr/u32 union mixes are ambiguous on 64-bit. */
    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 0x100u + (uint64_t)(slot - g_cli)};
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, fd, &ev);
}

/* ---------------------------------------------------------------- main */

static int make_listener(const char *port, bool unix_socket)
{
    int fd;
    if (unix_socket) {
        unlink(g_ctl_path);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        struct sockaddr_un addr = {.sun_family = AF_UNIX};
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_ctl_path);
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
            listen(fd, 4) < 0) {
            close(fd);
            return -1;
        }
        chmod(g_ctl_path, 0666);
    } else {
        fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            return -1;
        }
        static int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in addr = {.sin_family = AF_INET};
        addr.sin_port = htons((uint16_t)atoi(port));
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 4) < 0) {
            close(fd);
            return -1;
        }
    }
    set_nonblock(fd);
    return fd;
}

/* Optional TCP control endpoint, bound to loopback only. It speaks the exact
 * same line protocol as the Unix socket, so a GUI running on Windows can drive
 * the daemon in WSL without a second protocol implementation. */
static int make_loopback_listener(const char *port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    static int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET};
    addr.sin_port = htons((uint16_t)atoi(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 4) < 0) {
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
        } else if (strcmp(argv[i], "--ctl") == 0 && i + 1 < argc) {
            g_ctl_path = argv[++i];
        } else if (strcmp(argv[i], "--ctl-port") == 0 && i + 1 < argc) {
            const int p = atoi(argv[++i]);
            if (p <= 0 || p > 65535) {
                fprintf(stderr, "linsightd: bad ctl port '%s'\n", argv[i]);
                return 1;
            }
            g_ctl_port = argv[i];
        } else if (strcmp(argv[i], "--log-dir") == 0 && i + 1 < argc) {
            g_log_dir = argv[++i];
        } else {
            fprintf(stderr,
                    "usage: linsightd [--port N] [--ctl PATH] [--ctl-port N] "
                    "[--log-dir DIR]\n");
            return 1;
        }
    }
    mkdir(g_log_dir, 0755);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGPIPE);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    /* Nonblocking: the drain loops below must never block the main loop. */
    const int sigfd = signalfd(-1, &mask, SFD_NONBLOCK);

    const int tcp_fd = make_listener(g_port, false);
    const int unix_fd = make_listener(g_ctl_path, true);
    const int ctl_tcp_fd = g_ctl_port ? make_loopback_listener(g_ctl_port) : -1;
    if (tcp_fd < 0 || unix_fd < 0 || (g_ctl_port && ctl_tcp_fd < 0) || sigfd < 0) {
        fprintf(stderr, TAG ": listener setup failed: %s\n", strerror(errno));
        return 1;
    }

    g_epoll = epoll_create1(0);
    for (int i = 0; i < CLI_MAX; i++) {
        g_cli[i].fd = -1;
    }
    linsight_parser_init(&g_dev.parser, g_dev.payload, LINSIGHT_MAX_CAN_BATCH);
    if (g_epoll < 0) {
        fprintf(stderr, TAG ": epoll_create1 failed: %s\n", strerror(errno));
        return 1;
    }

    struct epoll_event ev = {0}; /* zero-init: u64 tags must not carry stack garbage */
    ev.events = EPOLLIN;
    ev.data.u64 = 1; /* tcp listener */
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, tcp_fd, &ev);
    ev.data.u64 = 2; /* unix listener */
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, unix_fd, &ev);
    if (ctl_tcp_fd >= 0) {
        ev.data.u64 = 5; /* loopback TCP control listener */
        epoll_ctl(g_epoll, EPOLL_CTL_ADD, ctl_tcp_fd, &ev);
    }
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

    pthread_t writer;
    if (pthread_create(&writer, NULL, writer_thread, NULL) != 0) {
        fprintf(stderr, TAG ": writer thread failed\n");
        return 1;
    }

    fprintf(stderr, TAG ": listening on :%s, ctl %s, logs %s\n", g_port, g_ctl_path,
            g_log_dir);
    if (ctl_tcp_fd >= 0) {
        fprintf(stderr, TAG ": tcp control on 127.0.0.1:%s\n", g_ctl_port);
    }

    uint64_t last_recs = 0;
    while (!g_stop) {
        struct epoll_event events[8];
        const int n = epoll_wait(g_epoll, events, 8, -1);
        for (int i = 0; i < n; i++) {
            const uint64_t tag = events[i].data.u64;
            cli_conn_t *cli = NULL;
            if (tag >= 0x100u && tag < 0x100u + CLI_MAX) {
                cli = &g_cli[tag - 0x100u];
            }

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
                pthread_mutex_lock(&g_ring.lock);
                const uint64_t written = g_ring.written;
                const uint64_t dropped = g_ring.dropped;
                pthread_mutex_unlock(&g_ring.lock);
                fprintf(stderr, TAG ": recs=%llu (+%llu) written=%llu dropped=%llu\n",
                        (unsigned long long)g_dev.recs,
                        (unsigned long long)(g_dev.recs - last_recs),
                        (unsigned long long)written, (unsigned long long)dropped);
                last_recs = g_dev.recs;
            } else if (tag == 1) {
                device_accept(tcp_fd);
            } else if (tag == 2) {
                cli_accept(unix_fd);
            } else if (tag == 5) {
                cli_accept(ctl_tcp_fd);
            } else if (tag == 0) { /* device socket */
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
            } else { /* cli conn */
                const ssize_t got =
                    recv(cli->fd, cli->buf + cli->len, sizeof(cli->buf) - (size_t)cli->len, 0);
                if (got <= 0) {
                    cli_close(cli);
                    continue;
                }
                cli->len += (int)got;
                if (memchr(cli->buf, '\n', (size_t)cli->len)) {
                    cli_dispatch(cli);
                }
            }
        }
    }

    device_close();
    for (int i = 0; i < CLI_MAX; i++) {
        if (g_cli[i].fd >= 0) {
            cli_close(&g_cli[i]);
        }
    }
    pthread_mutex_lock(&g_ring.lock);
    g_ring.done = true;
    pthread_cond_signal(&g_ring.cond);
    pthread_mutex_unlock(&g_ring.lock);
    pthread_join(writer, NULL);
    close(tcp_fd);
    close(unix_fd);
    if (ctl_tcp_fd >= 0) {
        close(ctl_tcp_fd);
    }
    unlink(g_ctl_path);
    fprintf(stderr, TAG ": exited cleanly\n");
    return 0;
}
