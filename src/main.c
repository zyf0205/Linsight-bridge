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

#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "device.h"
#include "discovery.h"
#include "epoll_tags.h"
#include "pacer.h"
#include "util.h"
#include "vcan.h"

#define TAG "linsightd"
#define LINSIGHT_HOST_VERSION "0.7.0"

#define MAX_EVENTS 16

static const char *g_port = "8888";
static int g_discovery_port = DISCOVERY_PORT_DEFAULT;
static int g_discovery_fd = -1;
static const char *g_vcan_ifname;
static bool g_paced = true;
static volatile sig_atomic_t g_stop;

/* ------------------------------------------------------- TCP device listener */

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

    pacer_init(g_paced);
    if (g_vcan_ifname && vcan_open(g_vcan_ifname) < 0) {
        return 1;
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
    const int epoll_fd = epoll_create1(0);

    if (tcp_fd < 0 || sigfd < 0 || tfd < 0 || epoll_fd < 0) {
        fprintf(stderr, TAG ": initialization failed: %s\n", strerror(errno));
        return 1;
    }

    device_init(epoll_fd);

    if (g_discovery_port != 0) {
        g_discovery_fd = discovery_open(g_discovery_port);
    }

    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = EPOLL_TAG_TCP_LISTEN};
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tcp_fd, &ev);
    ev.data.u64 = EPOLL_TAG_SIGNAL;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sigfd, &ev);
    if (g_discovery_fd >= 0) {
        ev.data.u64 = EPOLL_TAG_DISCOVERY;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_discovery_fd, &ev);
    }

    struct itimerspec its = {
        .it_interval = {.tv_sec = 1},
        .it_value = {.tv_sec = 1},
    };
    timerfd_settime(tfd, 0, &its, NULL);
    ev.data.u64 = EPOLL_TAG_TIMER;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tfd, &ev);

    fprintf(stderr, TAG ": listening on :%s", g_port);
    if (g_discovery_fd >= 0) {
        fprintf(stderr, ", discovery udp :%d", g_discovery_port);
    }
    if (vcan_is_open()) {
        fprintf(stderr, ", SocketCAN %s (%s)", g_vcan_ifname,
                pacer_enabled() ? "paced 1:1" : "burst");
    }
    fprintf(stderr, "\n");

    uint64_t last_recs = 0;
    while (!g_stop) {
        const int timeout = (vcan_is_open() && pacer_enabled())
                                ? pacer_epoll_timeout_ms(mono_ns())
                                : -1;
        struct epoll_event events[MAX_EVENTS];
        const int n = epoll_wait(epoll_fd, events, MAX_EVENTS, timeout);
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
                device_check_idle();
                const uint64_t recs = device_records();
                const uint64_t delta = recs >= last_recs ? recs - last_recs : recs;
                uint64_t written = 0;
                uint64_t dropped = 0;
                vcan_stats(&written, &dropped);
                fprintf(stderr,
                        TAG ": recs=%llu (+%llu) vcan=%llu vcan_dropped=%llu paceq=%u\n",
                        (unsigned long long)recs, (unsigned long long)delta,
                        (unsigned long long)written, (unsigned long long)dropped,
                        (unsigned)pacer_pending());
                last_recs = recs;
                break;
            }
            case EPOLL_TAG_DISCOVERY:
                discovery_handle(g_discovery_fd, g_port);
                break;
            case EPOLL_TAG_TCP_LISTEN:
                device_accept(tcp_fd);
                break;
            case EPOLL_TAG_DEVICE:
                device_event(events[i].events);
                break;
            }
        }
        pacer_pump();
    }

    device_close();
    pacer_drain();
    vcan_close();
    if (g_discovery_fd >= 0) {
        close(g_discovery_fd);
    }
    close(tcp_fd);
    close(tfd);
    close(sigfd);
    close(epoll_fd);
    fprintf(stderr, TAG ": exited cleanly\n");
    return 0;
}
