/*
 * linsightd — LinSight CAN host daemon (M2).
 *
 * epoll main loop: TCP listener (the device), Unix-socket listener (the CLI),
 * an optional loopback TCP listener (Windows GUI / remote CLI), a minimal
 * HTTP + SSE dashboard (web/, epoll clients, no threads), signalfd
 * (SIGTERM/SIGINT graceful exit) and a 1 Hz timerfd (stats).
 * One device session at a time; CAN_BATCH records flow into a bounded ring
 * drained by a writer thread that appends to per-session .lsc files.
 *
 * CLI commands (newline-terminated over the control socket):
 *   stats   -> one JSON status line, socket closes
 *   live    -> candump lines streamed until the client disconnects
 *   pause / resume / clear -> forwarded to the device as CONTROL, ACK echoed
 *   sim ... -> built-in traffic simulator (see sim.h), no device needed
 */

#include <dirent.h>
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
#include "http.h"
#include "session.h"
#include "sim.h"

#define TAG "linsightd"

#define WRING_CAP 131072u /* records; ~3 MB: absorbs a 4 MiB device backlog */
#define CLI_MAX 8
#define RX_BUF_SIZE 4096
#define IDLE_TIMEOUT_MS 30000u
#define LOG_BATCH 512u /* records per writer iteration */
#define DEV_OUT_CAP 4096u
#define CLI_OUT_CAP 4096u
#define MAX_EVENTS 64
#define ACK_TIMEOUT_MS 3000u

static const char *g_port = "8888";
static const char *g_ctl_path = "/tmp/linsight.sock";
static const char *g_ctl_port = NULL; /* optional loopback TCP control port */
static const char *g_log_dir = ".";
static uint64_t g_log_max_bytes; /* 0 = one file per session */
static uint64_t g_log_max_total_bytes; /* 0 = keep every log */
static char g_current_log[512];
static int g_http_port = 8080;    /* 0 disables the dashboard */
static const char *g_http_bind = "127.0.0.1";
static const char *g_web_dir = "web";
static http_server_t *g_http;

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

    /* Wall time corresponding to device uptime 0, captured from HELLO
     * rather than the first backlog record, so a reconnect does not shift
     * recorded history. */
    uint64_t anchor_wall_ms;
    uint32_t anchor_boot;
    uint32_t anchor_gen;
    char log_error[128];

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
static bool wring_take_open(uint32_t gen, char *path, size_t cap, uint64_t *start_wall_ms,
                            uint32_t *boot_count)
{
    bool taken = false;
    pthread_mutex_lock(&g_ring.lock);
    if (g_ring.open_req && g_ring.open_gen == gen) {
        snprintf(path, cap, "%s", g_ring.open_path);
        const bool anchored = g_ring.anchor_gen == gen;
        *start_wall_ms = anchored ? g_ring.anchor_wall_ms : 0;
        *boot_count = anchored ? g_ring.anchor_boot : 0;
        g_ring.open_req = false;
        taken = true;
    }
    pthread_mutex_unlock(&g_ring.lock);
    return taken;
}

static void wring_set_anchor_info(uint64_t start_wall_ms, uint32_t boot_count)
{
    pthread_mutex_lock(&g_ring.lock);
    g_ring.anchor_wall_ms = start_wall_ms;
    g_ring.anchor_boot = boot_count;
    g_ring.anchor_gen = g_ring.gen;
    pthread_mutex_unlock(&g_ring.lock);
}

static void wring_set_log_error(const char *msg)
{
    pthread_mutex_lock(&g_ring.lock);
    snprintf(g_ring.log_error, sizeof(g_ring.log_error), "%s", msg ? msg : "");
    pthread_mutex_unlock(&g_ring.lock);
}

/* Ask the writer to open a fresh session log. Callers: a real device session
 * and the simulator's first emit (no device). Bumping gen rotates: the writer
 * closes the previous file at the generation boundary. */
static void wring_request_open(const char *why)
{
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
    fprintf(stderr, TAG ": %s session, logging to %s\n", why, g_current_log);
}

/* Simulator-only helper: open one session log lazily on the first emitted
 * record, so `sim` traffic reaches the .lsc writer without a device. A real
 * device connection always takes precedence (sim_emit defers to its session),
 * and sim_allow_reopen() re-arms this after the device disconnects. */
static bool s_sim_log_open;

/* device_t lives further down; both helpers only need g_dev.fd. */
static bool device_connected(void);

static void sim_request_open(void)
{
    if (s_sim_log_open || device_connected()) {
        return;
    }
    s_sim_log_open = true;
    wring_request_open("sim");
}

static void sim_allow_reopen(void)
{
    s_sim_log_open = false;
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

/* Current .lsc file, published by the writer thread for stats/CLI. */
static void wring_set_current_log(const char *path)
{
    pthread_mutex_lock(&g_ring.lock);
    snprintf(g_current_log, sizeof(g_current_log), "%s", path);
    pthread_mutex_unlock(&g_ring.lock);
}

/* `base` is the per-session path; rotated parts keep the session name and add
 * -pNNN before the extension. */
static void log_part_path(const char *base, uint32_t part, char *out, size_t cap)
{
    if (part == 0) {
        snprintf(out, cap, "%s", base);
        return;
    }
    const char *dot = strrchr(base, '.');
    if (dot) {
        snprintf(out, cap, "%.*s-p%03u%s", (int)(dot - base), base, (unsigned)part, dot);
    } else {
        snprintf(out, cap, "%s-p%03u", base, (unsigned)part);
    }
}

static uint64_t mono_ms(void);

static int log_filter(const struct dirent *e)
{
    const size_t n = strlen(e->d_name);
    return n > 8 && strncmp(e->d_name, "can-", 4) == 0 &&
           strcmp(e->d_name + n - 4, ".lsc") == 0;
}

/* Delete the oldest can-*.lsc files until the directory is under the cap.
 * Names sort by their timestamp prefix, so alphasort is oldest-first. */
static void log_retention(void)
{
    if (g_log_max_total_bytes == 0) {
        return;
    }
    struct dirent **list = NULL;
    const int n = scandir(g_log_dir, &list, log_filter, alphasort);
    if (n <= 0) {
        return;
    }

    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        char path[600];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", g_log_dir, list[i]->d_name);
        if (stat(path, &st) == 0 && st.st_size > 0) {
            total += (uint64_t)st.st_size;
        }
    }

    pthread_mutex_lock(&g_ring.lock);
    char current[512];
    snprintf(current, sizeof(current), "%s", g_current_log);
    pthread_mutex_unlock(&g_ring.lock);

    for (int i = 0; i < n && total > g_log_max_total_bytes; i++) {
        char path[600];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", g_log_dir, list[i]->d_name);
        if (strcmp(path, current) == 0) {
            continue; /* never delete the file being written */
        }
        if (stat(path, &st) != 0) {
            continue;
        }
        if (unlink(path) == 0) {
            const uint64_t sz = st.st_size > 0 ? (uint64_t)st.st_size : 0;
            total = sz > total ? 0 : total - sz;
        }
    }
    for (int i = 0; i < n; i++) {
        free(list[i]);
    }
    free(list);
}

static void *writer_thread(void *arg)
{
    (void)arg;
    lsc_writer_t w = {0};
    bool log_open = false;
    uint32_t log_gen = 0;
    char base_path[512] = {0};
    uint64_t log_start_wall_ms = 0;
    uint32_t log_boot = 0;
    uint32_t log_part = 0;
    char reopen_path[560] = {0};
    uint64_t reopen_after_ms = 0;

    log_retention();

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

            /* A transient write error closed the file: retry a fresh part
             * every 2 s instead of dropping the rest of the session. */
            if (!log_open && reopen_path[0] && e->gen == log_gen &&
                (reopen_after_ms == 0 || mono_ms() >= reopen_after_ms)) {
                const lsc_open_info_t info = {
                    .start_wall_ms = log_start_wall_ms,
                    .flags = LSC_FLAG_ANCHOR_MS_VALID | LSC_FLAG_BOOT_VALID,
                    .boot_count = log_boot,
                };
                log_open = lsc_writer_open_ex(&w, reopen_path, wring_bitrate(), &info) == 0;
                if (log_open) {
                    wring_set_current_log(reopen_path);
                    wring_set_log_error("");
                    reopen_path[0] = '\0';
                } else {
                    reopen_after_ms = mono_ms() + 2000u;
                }
            }

            if (log_open && e->gen != log_gen) {
                /* Generation boundary: every record of the old session has
                 * been written, so it is safe to close its file now. */
                lsc_writer_close(&w);
                log_open = false;
                wring_take_close(log_gen);
            }
            if (!log_open) {
                char path[512];
                uint64_t start_wall_ms = 0;
                uint32_t boot_count = 0;
                if (wring_take_open(e->gen, path, sizeof(path), &start_wall_ms, &boot_count)) {
                    if (!start_wall_ms) {
                        /* No HELLO uptime (old firmware): map this record's
                         * uptime onto the current wall clock. */
                        struct timespec ts;
                        clock_gettime(CLOCK_REALTIME, &ts);
                        const uint64_t now_ms = (uint64_t)ts.tv_sec * 1000u +
                                                (uint64_t)ts.tv_nsec / 1000000u;
                        start_wall_ms = now_ms > e->rec.ts_ms ? now_ms - e->rec.ts_ms : 0;
                    }
                    const lsc_open_info_t info = {
                        .start_wall_ms = start_wall_ms,
                        .flags = LSC_FLAG_ANCHOR_MS_VALID | LSC_FLAG_BOOT_VALID,
                        .boot_count = boot_count,
                    };
                    log_open = lsc_writer_open_ex(&w, path, wring_bitrate(), &info) == 0;
                    log_gen = e->gen;
                    wring_clear_close_before(log_gen);
                    if (log_open) {
                        snprintf(base_path, sizeof(base_path), "%s", path);
                        log_start_wall_ms = start_wall_ms;
                        log_boot = boot_count;
                        log_part = 0;
                        reopen_path[0] = '\0';
                        reopen_after_ms = 0;
                        wring_set_current_log(base_path);
                        wring_set_log_error("");
                    } else {
                        wring_set_log_error("open failed");
                        fprintf(stderr, TAG ": cannot open %s: %s\n", path, strerror(errno));
                    }
                }
                if (!log_open) {
                    /* The open request was superseded (quick reconnect) or the
                     * file could not be created: drop this stale generation. */
                    continue;
                }
            }

            /* Size cap: start a new part before this record would cross it.
             * The session anchor stays the same, so timestamps remain a single
             * continuous uptime axis across the parts. */
            if (log_open && g_log_max_bytes > 0 &&
                (uint64_t)LSC_HEADER_SIZE + (w.written + 1) * LINSIGHT_REC_SIZE >
                    g_log_max_bytes) {
                char next[560];
                lsc_writer_close(&w);
                log_open = false;
                log_part++;
                log_part_path(base_path, log_part, next, sizeof(next));
                const lsc_open_info_t info = {
                    .start_wall_ms = log_start_wall_ms,
                    .flags = LSC_FLAG_ANCHOR_MS_VALID | LSC_FLAG_BOOT_VALID,
                    .boot_count = log_boot,
                };
                log_open = lsc_writer_open_ex(&w, next, wring_bitrate(), &info) == 0;
                if (log_open) {
                    wring_set_current_log(next);
                    wring_set_log_error("");
                    fprintf(stderr, TAG ": log rotated to %s\n", next);
                } else {
                    wring_set_log_error("rotate failed");
                    fprintf(stderr, TAG ": cannot rotate log to %s: %s\n", next,
                            strerror(errno));
                }
            }

            if (lsc_writer_write(&w, &e->rec) != 0) {
                fprintf(stderr, TAG ": log write failed: %s\n", strerror(errno));
                wring_set_log_error("write failed");
                /* Close the bad stream and try a fresh part in 2 s; one
                 * transient error must not discard the rest of the session. */
                lsc_writer_close(&w);
                log_open = false;
                if (base_path[0]) {
                    log_part++;
                    log_part_path(base_path, log_part, reopen_path, sizeof(reopen_path));
                }
                reopen_after_ms = mono_ms() + 2000u;
                continue; /* this record is counted as dropped */
            }
            ok++;
        }

        if (ok < n) {
            /* Includes log-open failures, stale generations and write errors:
             * `dropped` is the total number of records that never reached a
             * .lsc file, not just ring overflow. */
            pthread_mutex_lock(&g_ring.lock);
            g_ring.dropped += n - ok;
            pthread_mutex_unlock(&g_ring.lock);
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
                log_retention();
            }
            if (!log_open && wring_take_close(log_gen)) {
                reopen_path[0] = '\0';
                reopen_after_ms = 0;
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
    uint64_t recs;   /* raw records received this session */
    char stats[513]; /* latest STATS JSON from the device */
    char info[513];  /* latest DEVICE_INFO JSON */
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

typedef struct {
    int fd;
    uint8_t buf[128];
    int len;
    bool live;
    bool filter_id_valid;
    uint32_t filter_id;
    int filter_ext; /* -1 = any, 0 = standard only, 1 = extended only */
    uint16_t pending_id; /* CONTROL id awaiting ACK, 0 = none */
    uint64_t ack_deadline_ms;

    uint8_t out[CLI_OUT_CAP];
    size_t out_off;
    size_t out_len;
    bool close_after; /* close once the output queue drains */
} cli_conn_t;

static device_t g_dev = {.fd = -1};
static cli_conn_t g_cli[CLI_MAX];
static int g_epoll = -1;
static volatile sig_atomic_t g_stop;
static uint16_t g_ctrl_id;

static bool device_connected(void)
{
    return g_dev.fd >= 0;
}
static uint64_t g_session_start_ms;

static void set_nonblock(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ------------------------------------------------------- output queues */

static void cli_close(cli_conn_t *c);
static void device_close(void);

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

static void cli_update_epoll(cli_conn_t *c)
{
    if (c->fd < 0) {
        return;
    }
    struct epoll_event ev = {
        .events = EPOLLIN | (c->out_len > c->out_off ? EPOLLOUT : 0),
        .data.u64 = 0x100u + (uint64_t)(c - g_cli),
    };
    epoll_ctl(g_epoll, EPOLL_CTL_MOD, c->fd, &ev);
}

/* Append one complete device frame. CONTROL/PONG are tiny; if the queue is
 * full the frame is dropped rather than blocking the epoll loop. */
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

static bool cli_queue(cli_conn_t *c, const void *data, size_t len)
{
    if (c->fd < 0) {
        return false;
    }
    if (c->out_off > 0) {
        memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
        c->out_len -= c->out_off;
        c->out_off = 0;
    }
    if (len > sizeof(c->out) - c->out_len) {
        return false;
    }
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
    cli_update_epoll(c);
    return true;
}

static void cli_flush(cli_conn_t *c)
{
    if (c->fd < 0) {
        return;
    }
    while (c->out_off < c->out_len) {
        const ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off,
                               MSG_NOSIGNAL);
        if (n > 0) {
            c->out_off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        cli_close(c);
        return;
    }
    if (c->out_off == c->out_len) {
        c->out_off = 0;
        c->out_len = 0;
        if (c->close_after) {
            cli_close(c);
            return;
        }
    }
    cli_update_epoll(c);
}

static void cli_reply(cli_conn_t *c, const char *text)
{
    if (!cli_queue(c, text, strlen(text))) {
        cli_close(c);
        return;
    }
    c->close_after = true;
}

static bool send_control_fields(const char *action, const char *fields)
{
    char json[LINSIGHT_MAX_CONTROL + 1];
    const bool has_fields = fields && fields[0];
    const int n = snprintf(json, sizeof(json), "{\"id\":%u,\"action\":\"%s\"%s%s}",
                           (unsigned)++g_ctrl_id, action, has_fields ? "," : "",
                           has_fields ? fields : "");
    if (n <= 0 || n > (int)LINSIGHT_MAX_CONTROL) {
        return false;
    }
    return device_queue_frame(LINSIGHT_FRAME_CONTROL, json, (uint16_t)n);
}

static bool send_control(const char *action)
{
    return send_control_fields(action, NULL);
}

/* ------------------------------------------------------------- frames */

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
                len = canlog_format_candump_ms(&recs[i], g_session_start_ms, line,
                                               sizeof(line) - 1);
                formatted = true;
                if (len <= 0) {
                    break;
                }
                line[len++] = '\n';
            }
            /* Best-effort: queue for EPOLLOUT. A slow `live` consumer must
             * never stall the device RX/writer path; a full queue drops this
             * line rather than blocking or closing the client. */
            (void)cli_queue(cli, line, (size_t)len);
        }
    }

    /* Web dashboard gets the unfiltered batch; filtering is client-side. */
    if (g_http) {
        http_broadcast_records(g_http, recs, n);
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

    /* Dedupe against a reconnect backlog and count gaps; only accepted
     * records reach the writer and live clients. */
    uint32_t accepted = 0;
    for (uint32_t i = 0; i < valid; i++) {
        if (session_record_new(&g_dev.sess, &recs[i])) {
            recs[accepted++] = recs[i];
        }
    }
    g_dev.recs += valid;

    if (accepted) {
        if (!g_dev.anchor_valid) {
            /* Old firmware without HELLO uptime: map the first record's
             * uptime onto the current wall clock. */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            const uint64_t now_ms = (uint64_t)ts.tv_sec * 1000u +
                                    (uint64_t)ts.tv_nsec / 1000000u;
            g_dev.anchor_wall_ms =
                now_ms > recs[0].ts_ms ? now_ms - recs[0].ts_ms : 0;
            g_dev.anchor_valid = true;
            g_session_start_ms = g_dev.anchor_wall_ms;
            wring_set_anchor_info(g_dev.anchor_wall_ms, g_dev.boot_count);
        }
        wring_push(recs, accepted);
        deliver_live(recs, accepted);
    }
}

/* Simulator ingest: bypass device-session dedupe/anchor (those guard real
 * reconnect backlogs) but reuse the writer ring, live clients and SSE, so
 * simulated traffic is indistinguishable from device traffic downstream. */
static void sim_emit(void *ctx, const linsight_can_record_t *recs, uint32_t n)
{
    (void)ctx;
    if (!g_dev.anchor_valid) {
        /* Same wall-clock mapping as a first batch from old firmware. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        const uint64_t now_ms = (uint64_t)ts.tv_sec * 1000u +
                                (uint64_t)ts.tv_nsec / 1000000u;
        g_dev.anchor_wall_ms = now_ms > recs[0].ts_ms ? now_ms - recs[0].ts_ms : 0;
        g_dev.anchor_valid = true;
        g_session_start_ms = g_dev.anchor_wall_ms;
        wring_set_anchor_info(g_dev.anchor_wall_ms, g_dev.boot_count);
    }
    if (g_dev.fd < 0) {
        /* No device session owns the writer: open a sim session log so the
         * records land in a .lsc instead of overflowing the ring. When a real
         * device connects later its own open request rotates to a new file. */
        sim_request_open();
    }
    g_dev.recs += n;
    wring_push(recs, n);
    deliver_live(recs, n);
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
        if (h.bitrate) {
            wring_set_bitrate(h.bitrate);
        }
        if (h.has_device_id) {
            snprintf(g_dev.device_id, sizeof(g_dev.device_id), "%s", h.device_id);
        }
        g_dev.boot_count = h.boot_count;
        session_reset(&g_dev.sess, h.boot_count, h.has_boot);
        if (h.has_uptime) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            const uint64_t now_ms = (uint64_t)ts.tv_sec * 1000u +
                                    (uint64_t)ts.tv_nsec / 1000000u;
            g_dev.anchor_wall_ms =
                session_anchor_start_wall_ms(now_ms, h.uptime_ms);
            g_dev.anchor_valid = true;
            g_session_start_ms = g_dev.anchor_wall_ms;
            wring_set_anchor_info(g_dev.anchor_wall_ms, h.boot_count);
        }
        break;
    }
    case LINSIGHT_FRAME_CAN_BATCH:
        handle_batch(payload, len);
        break;
    case LINSIGHT_FRAME_STATS:
        if (len <= LINSIGHT_MAX_STATS) {
            memcpy(g_dev.stats, payload, len);
            g_dev.stats[len] = '\0';
        }
        break;
    case LINSIGHT_FRAME_DEVICE_INFO:
        if (len <= LINSIGHT_MAX_DEVICE_INFO) {
            memcpy(g_dev.info, payload, len);
            g_dev.info[len] = '\0';
        }
        break;
    case LINSIGHT_FRAME_EVENT:
        fprintf(stderr, TAG ": device event: %.*s\n", (int)len, (const char *)payload);
        break;
    case LINSIGHT_FRAME_PING:
        (void)device_queue_frame(LINSIGHT_FRAME_PONG, payload, len);
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
                g_cli[c].ack_deadline_ms = 0;
                char reply[80];
                const int rn = snprintf(reply, sizeof(reply), "%s\n", ack);
                if (rn <= 0 || !cli_queue(&g_cli[c], reply, (size_t)rn)) {
                    cli_close(&g_cli[c]);
                } else {
                    g_cli[c].close_after = true; /* one reply, then EOF */
                }
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
    g_dev.out_off = 0;
    g_dev.out_len = 0;
    g_dev.anchor_valid = false; /* a later sim_emit re-anchors its own session */
    fprintf(stderr, TAG ": device session ended (%llu records)\n",
            (unsigned long long)g_dev.recs);
    pthread_mutex_lock(&g_ring.lock);
    g_ring.close_req = true;
    g_ring.close_gen = g_ring.gen;
    g_current_log[0] = '\0';
    pthread_cond_signal(&g_ring.cond);
    pthread_mutex_unlock(&g_ring.lock);
    /* The sim session may outlive this device session: let its next emit
     * open a fresh log instead of piling onto a closed generation. */
    sim_allow_reopen();
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
    g_dev.info[0] = '\0';
    g_dev.anchor_valid = false;
    session_clear_counters(&g_dev.sess);
    linsight_parser_reset(&g_dev.parser);
    g_session_start_ms = 0;

    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 0}; /* u64 0 = device */
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, fd, &ev);

    wring_request_open("device");
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
    c->ack_deadline_ms = 0;
    c->out_off = 0;
    c->out_len = 0;
    c->close_after = false;
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

static size_t build_stats_json(char *out, size_t cap)
{
    char log_path[512];
    char log_error[128];
    pthread_mutex_lock(&g_ring.lock);
    const uint64_t written = g_ring.written;
    const uint64_t dropped = g_ring.dropped;
    snprintf(log_path, sizeof(log_path), "%s", g_current_log);
    snprintf(log_error, sizeof(log_error), "%s", g_ring.log_error);
    pthread_mutex_unlock(&g_ring.lock);

    sim_status_t sim;
    sim_status(&sim);
    char sim_json[160];
    const int sn = snprintf(sim_json, sizeof(sim_json),
                            "{\"running\":%s,\"pattern\":\"%s\",\"rate\":%u,"
                            "\"generated\":%llu}",
                            sim.running ? "true" : "false",
                            sim_pattern_name(sim.pattern), sim.rate_fps,
                            (unsigned long long)sim.generated);
    if (sn < 0 || (size_t)sn >= sizeof(sim_json)) {
        sim_json[0] = '\0';
    }

    const int n = snprintf(
        out, cap,
        "{\"version\":\"0.3.0\",\"device\":%s,\"device_id\":\"%s\",\"boot\":%u,"
        "\"records\":%llu,\"recv_raw\":%llu,\"backlog_skipped\":%llu,"
        "\"seq_gaps\":%llu,\"seq_missing\":%llu,\"seq_ambiguous\":%llu,"
        "\"written\":%llu,\"dropped\":%llu,\"log\":\"%s\",\"log_error\":\"%s\","
        "\"anchor_wall_ms\":%llu,\"stats\":%s,\"info\":%s,\"sim\":%s}",
        g_dev.fd >= 0 ? "true" : "false", g_dev.device_id, (unsigned)g_dev.boot_count,
        /* "records" counts device-session accepted records + simulated ones;
         * recv_raw stays the raw device-side counter. */
        (unsigned long long)(g_dev.sess.accepted + sim.generated),
        (unsigned long long)g_dev.recs,
        (unsigned long long)g_dev.sess.skipped, (unsigned long long)g_dev.sess.gaps,
        (unsigned long long)g_dev.sess.missing, (unsigned long long)g_dev.sess.ambiguous,
        (unsigned long long)written, (unsigned long long)dropped,
        log_path[0] ? log_path : "", log_error,
        (unsigned long long)g_dev.anchor_wall_ms,
        g_dev.stats[0] ? g_dev.stats : "{}", g_dev.info[0] ? g_dev.info : "{}",
        sim_json[0] ? sim_json : "{}");
    if (n <= 0 || (size_t)n >= cap) {
        return 0;
    }
    return (size_t)n;
}

/* Shared simulator command parser for the CLI and the web dashboard.
 * Grammar (space separated):
 *   sim start <pattern> [rate]   patterns: engine|bms|mix|counter, rate 1..1000
 *   sim stop
 *   sim status
 *   sim inject <id> [ext] [rtr] [dlc] [hexbytes]   id hex, e.g. 0x1AB
 * Returns false and fills err on a bad command. */
static bool sim_command(const char *args, char *err, size_t errcap)
{
    char pattern[16] = {0};
    unsigned rate = 0;

    if (strcmp(args, "stop") == 0) {
        sim_stop();
        return true;
    }
    if (strcmp(args, "status") == 0 || args[0] == '\0') {
        return true; /* status rides on the stats JSON both callers print */
    }
    if (sscanf(args, "start %15s %u", pattern, &rate) == 2 ||
        sscanf(args, "start %15s", pattern) == 1) {
        if (rate == 0) {
            rate = 50u; /* default: a busy-looking but gentle bus */
        }
        if (!sim_start(pattern, rate, sim_emit, NULL)) {
            sim_status_t st;
            sim_status(&st);
            snprintf(err, errcap, "%s", st.last_error[0] ? st.last_error : "start failed");
            return false;
        }
        return true;
    }
    if (strncmp(args, "inject ", 7) == 0) {
        const char *p = args + 7;
        char *end = NULL;
        const unsigned long id = strtoul(p, &end, 16);
        if (end == p || id > 0x1FFFFFFFUL) {
            snprintf(err, errcap, "bad id");
            return false;
        }
        bool ext = id > 0x7FFUL;
        bool rtr = false;
        p = end;
        while (*p == ' ') {
            p++;
        }
        if (strncmp(p, "ext", 3) == 0) {
            ext = true;
            p += 3;
        }
        while (*p == ' ') {
            p++;
        }
        if (strncmp(p, "rtr", 3) == 0) {
            rtr = true;
            p += 3;
        }
        uint8_t data[LINSIGHT_DLC_MAX] = {0};
        uint8_t dlc = 0;
        if (!rtr) {
            for (; dlc < LINSIGHT_DLC_MAX && p[0] && p[1]; dlc++) {
                const unsigned byte = (unsigned)strtoul(p, &end, 16);
                if (end != p + 2 || byte > 0xFFu) {
                    snprintf(err, errcap, "bad data hex");
                    return false;
                }
                data[dlc] = (uint8_t)byte;
                p = end;
                while (*p == ' ') {
                    p++;
                }
            }
        }
        if (!sim_inject((uint32_t)id, ext, rtr, dlc, data, sim_emit, NULL)) {
            snprintf(err, errcap, "inject failed");
            return false;
        }
        return true;
    }
    snprintf(err, errcap, "bad sim command");
    return false;
}

/* Dashboard control endpoint: same whitelist as the CLI, ACK still goes to the
 * device (the web UI observes the resulting STATS). */
static bool web_control(void *ctx, const char *cmd, char *err, size_t errcap)
{
    (void)ctx;
    if (strcmp(cmd, "pause") == 0 || strcmp(cmd, "resume") == 0 ||
        strcmp(cmd, "clear") == 0) {
        if (g_dev.fd < 0) {
            snprintf(err, errcap, "no device");
            return false;
        }
        send_control(strcmp(cmd, "clear") == 0 ? "clear_stats" : cmd);
        return true;
    }
    if (strncmp(cmd, "sim", 3) == 0 &&
        (cmd[3] == '\0' || cmd[3] == ' ')) {
        return sim_command(cmd[3] == ' ' ? cmd + 4 : "", err, errcap);
    }
    if (strncmp(cmd, "config ", 7) == 0) {
        char fields[96];
        if (!build_config_fields(cmd + 7, fields, sizeof(fields))) {
            snprintf(err, errcap, "bad config");
            return false;
        }
        if (g_dev.fd < 0) {
            snprintf(err, errcap, "no device");
            return false;
        }
        if (!send_control_fields(LINSIGHT_CTRL_CONFIG, fields)) {
            snprintf(err, errcap, "send failed");
            return false;
        }
        return true;
    }
    snprintf(err, errcap, "unknown command");
    return false;
}

static size_t web_stats(void *ctx, char *out, size_t cap)
{
    (void)ctx;
    return build_stats_json(out, cap);
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
        char out[8192];
        size_t out_len = build_stats_json(out, sizeof(out) - 1);
        if (out_len == 0) {
            cli_reply(c, "ERR stats unavailable\n");
            return;
        }
        out[out_len++] = '\n';
        if (!cli_queue(c, out, out_len)) {
            cli_close(c);
            return;
        }
        c->close_after = true;
    } else if (strcmp(cmd, "live") == 0 ||
               (strncmp(cmd, "live ", 5) == 0)) {
        if (!parse_live_filters(c, cmd + 4)) {
            cli_reply(c, "ERR bad live filter\n");
            return;
        }
        c->live = true; /* records stream until the client goes away */
    } else if (strncmp(cmd, "sim", 3) == 0 &&
               (cmd[3] == '\0' || cmd[3] == ' ')) {
        char err[128] = {0};
        if (!sim_command(cmd[3] == ' ' ? cmd + 4 : "", err, sizeof(err))) {
            cli_reply(c, err[0] ? err : "ERR sim failed\n");
            return;
        }
        /* Echo the command plus the resulting state; the full sim JSON rides
         * on `stats` / /api/stats. */
        sim_status_t st;
        sim_status(&st);
        char out[192];
        int n;
        if (strncmp(cmd, "inject", 6) == 0) {
            n = snprintf(out, sizeof(out), "OK inject");
        } else if (strncmp(cmd, "start", 5) == 0) {
            n = snprintf(out, sizeof(out), "OK sim %s @ %u fps", sim_pattern_name(st.pattern),
                         st.rate_fps);
        } else {
            n = snprintf(out, sizeof(out), "OK sim %s",
                         st.running ? "running" : "stopped");
        }
        if (n > 0 && (size_t)n < sizeof(out) - 1) {
            n += snprintf(out + n, sizeof(out) - (size_t)n, " (%llu generated)\n",
                          (unsigned long long)st.generated);
        }
        (void)n;
        cli_reply(c, out);
    } else if (strncmp(cmd, "config", 6) == 0 &&
               (cmd[6] == '\0' || cmd[6] == ' ')) {
        char fields[96];
        if (!build_config_fields(cmd + 6, fields, sizeof(fields))) {
            cli_reply(c, "ERR bad config\n");
            return;
        }
        if (g_dev.fd < 0) {
            cli_reply(c, "ERR no device\n");
            return;
        }
        if (!send_control_fields(LINSIGHT_CTRL_CONFIG, fields)) {
            cli_reply(c, "ERR send failed\n");
            return;
        }
        c->pending_id = g_ctrl_id;
        c->ack_deadline_ms = mono_ms() + ACK_TIMEOUT_MS;
    } else if (strcmp(cmd, "pause") == 0 || strncmp(cmd, "pause ", 6) == 0 ||
               strcmp(cmd, "resume") == 0 || strcmp(cmd, "clear") == 0) {
        if (g_dev.fd < 0) {
            cli_reply(c, "ERR no device\n");
            return;
        }
        /* Device protocol names the action "clear_stats"; pause may carry an
         * optional lease so a dead host cannot leave the device paused. */
        const char *action = strcmp(cmd, "clear") == 0 ? "clear_stats" : cmd;
        char fields[48] = {0};
        const char *lease = strstr(cmd, "lease=");
        if (lease) {
            char *end = NULL;
            const unsigned long ms = strtoul(lease + 6, &end, 10);
            if (end == lease + 6 || (*end && *end != ' ') ||
                ms < LINSIGHT_PAUSE_LEASE_MIN_MS || ms > LINSIGHT_PAUSE_LEASE_MAX_MS) {
                cli_reply(c, "ERR bad lease\n");
                return;
            }
            snprintf(fields, sizeof(fields), "\"lease_ms\":%lu", ms);
            action = "pause";
        }
        if (!send_control_fields(action, fields[0] ? fields : NULL)) {
            cli_reply(c, "ERR send failed\n");
            return;
        }
        c->pending_id = g_ctrl_id;
        c->ack_deadline_ms = mono_ms() + ACK_TIMEOUT_MS;
    } else {
        cli_reply(c, "ERR unknown command\n");
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
        } else if (strcmp(argv[i], "--log-max-mb") == 0 && i + 1 < argc) {
            char *end = NULL;
            const unsigned long long mb = strtoull(argv[++i], &end, 10);
            if (end == argv[i] || *end || mb > 1024ull * 1024ull) {
                fprintf(stderr, "linsightd: bad log size '%s'\n", argv[i]);
                return 1;
            }
            g_log_max_bytes = mb * 1024ull * 1024ull; /* 0 keeps one file/session */
        } else if (strcmp(argv[i], "--log-max-total-mb") == 0 && i + 1 < argc) {
            char *end = NULL;
            const unsigned long long mb = strtoull(argv[++i], &end, 10);
            if (end == argv[i] || *end || mb > 1024ull * 1024ull) {
                fprintf(stderr, "linsightd: bad total log size '%s'\n", argv[i]);
                return 1;
            }
            g_log_max_total_bytes = mb * 1024ull * 1024ull;
        } else if (strcmp(argv[i], "--http-port") == 0 && i + 1 < argc) {
            const int p = atoi(argv[++i]);
            if (p < 0 || p > 65535) {
                fprintf(stderr, "linsightd: bad http port '%s'\n", argv[i]);
                return 1;
            }
            g_http_port = p; /* 0 disables the dashboard */
        } else if (strcmp(argv[i], "--http-bind") == 0 && i + 1 < argc) {
            g_http_bind = argv[++i];
        } else if (strcmp(argv[i], "--web-dir") == 0 && i + 1 < argc) {
            g_web_dir = argv[++i];
        } else {
            fprintf(stderr,
                    "usage: linsightd [--port N] [--ctl PATH] [--ctl-port N] "
                    "[--log-dir DIR]\n"
                    "                  [--log-max-mb N] [--log-max-total-mb N] "
                    "[--http-port N] [--http-bind ADDR] [--web-dir DIR]\n");
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

    /* 10 Hz simulator tick: emits frames into the same ingest path as the
     * device; no-op (one epoll wakeup per 100 ms) while not running. */
    const int simfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (simfd < 0) {
        fprintf(stderr, TAG ": sim timerfd failed: %s\n", strerror(errno));
        return 1;
    }
    struct itimerspec sim_its = {
        .it_interval = {.tv_nsec = 100 * 1000 * 1000L},
        .it_value = {.tv_nsec = 100 * 1000 * 1000L},
    };
    timerfd_settime(simfd, 0, &sim_its, NULL);
    ev.data.u64 = 6; /* sim tick */
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, simfd, &ev);

    if (g_http_port > 0) {
        g_http = http_server_new(g_epoll, g_http_bind, (uint16_t)g_http_port, g_web_dir,
                                 web_stats, web_control, NULL);
        if (!g_http) {
            fprintf(stderr, TAG ": web dashboard disabled (listen failed)\n");
        }
    }

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
    if (g_http) {
        fprintf(stderr, TAG ": web dashboard on http://%s:%d (web-dir %s)\n",
                g_http_bind, g_http_port, g_web_dir);
    }

    uint64_t last_recs = 0;
    while (!g_stop) {
        struct epoll_event events[MAX_EVENTS];
        const int n = epoll_wait(g_epoll, events, MAX_EVENTS, -1);
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

                /* A device that never ACKs must not leave the CLI hanging
                 * until its own 5 s receive timeout. */
                const uint64_t now_m = mono_ms();
                for (int c = 0; c < CLI_MAX; c++) {
                    cli_conn_t *cc = &g_cli[c];
                    if (cc->fd < 0 || cc->pending_id == 0 || cc->ack_deadline_ms == 0 ||
                        now_m < cc->ack_deadline_ms) {
                        continue;
                    }
                    cc->pending_id = 0;
                    cc->ack_deadline_ms = 0;
                    cli_reply(cc, "ERR ack timeout\n");
                }
            } else if (tag == 6) { /* 10 Hz simulator tick */
                uint64_t misses;
                while (read(simfd, &misses, sizeof(misses)) == sizeof(misses)) {
                }
                sim_tick(sim_emit, NULL);
            } else if (tag == 1) {
                device_accept(tcp_fd);
            } else if (tag == 2) {
                cli_accept(unix_fd);
            } else if (tag == 5) {
                cli_accept(ctl_tcp_fd);
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
            } else if (tag == HTTP_TAG_LISTEN) {
                http_server_accept(g_http);
            } else if (tag >= HTTP_TAG_CLIENT_BASE &&
                       tag < HTTP_TAG_CLIENT_BASE + HTTP_MAX_CLIENTS) {
                http_server_handle(g_http, (int)(tag - HTTP_TAG_CLIENT_BASE),
                                   events[i].events);
            } else { /* cli conn */
                const uint32_t revents = events[i].events;
                if (revents & EPOLLOUT) {
                    cli_flush(cli);
                }
                if (cli->fd < 0) {
                    continue;
                }
                if (!(revents & (EPOLLIN | EPOLLERR | EPOLLHUP))) {
                    continue;
                }
                const ssize_t got =
                    recv(cli->fd, cli->buf + cli->len, sizeof(cli->buf) - (size_t)cli->len, 0);
                if (got <= 0) {
                    if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        continue;
                    }
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
    http_server_free(g_http);
    g_http = NULL;
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
