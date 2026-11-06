/*
 * linsight-cli — control / inspect a running linsightd.
 *
 *   linsight-cli stats [--watch] [--interval MS]
 *   linsight-cli live [--id ID] [--ext|--std] [--color]
 *   linsight-cli config [--bitrate 125000|250000|500000|1000000]
 *                       [--mode listen|normal|selftest]
 *   linsight-cli pause | resume | clear
 *   linsight-cli export asc|candump FILE.lsc [-o OUT]
 *                       [--id ID] [--ext|--std] [--from EPOCH] [--to EPOCH]
 *   linsight-cli replay FILE.lsc [--iface can0] [--speed 1.0]
 *   linsight-cli compare REF.lsc CAND.lsc
 *
 * All commands use the daemon's control socket; `export`, `replay` and
 * `compare` read files directly and therefore work without a running daemon.
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "canlog.h"
#include "compare.h"
#include "replay.h"

#define DEFAULT_CTL "/tmp/linsight.sock"
#define REPLY_MAX 4096

static const char *g_ctl = DEFAULT_CTL;
static volatile sig_atomic_t g_stop;

static void usage(void)
{
    fprintf(stderr,
            "usage: linsight-cli stats [--watch] [--interval MS]\n"
            "       linsight-cli live [--id ID] [--ext|--std] [--color]\n"
            "       linsight-cli config [--bitrate N] [--mode listen|normal|selftest]\n"
            "       linsight-cli pause|resume|clear\n"
            "       linsight-cli export asc|candump FILE.lsc [-o OUT]\n"
            "                         [--id ID] [--ext|--std] [--from EPOCH] [--to EPOCH]\n"
            "       linsight-cli replay FILE.lsc [--iface can0] [--speed 1.0]\n"
            "       linsight-cli compare REF.lsc CAND.lsc\n"
            "       global: --ctl PATH (default " DEFAULT_CTL ")\n");
}

static int ctl_connect(void)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_ctl);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int connect_or_report(void)
{
    const int fd = ctl_connect();
    if (fd < 0) {
        fprintf(stderr, "linsight-cli: cannot connect to %s: %s\n", g_ctl,
                strerror(errno));
        fprintf(stderr, "(is linsightd running?)\n");
    }
    return fd;
}

static int send_command(int fd, const char *cmd)
{
    char msg[128];
    const int n = snprintf(msg, sizeof(msg), "%s\n", cmd);
    if (n <= 0 || n >= (int)sizeof(msg) || write(fd, msg, (size_t)n) != n) {
        fprintf(stderr, "linsight-cli: send failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* One-shot command whose reply is printed verbatim until the daemon closes. */
static int ctl_cmd(const char *cmd)
{
    const int fd = connect_or_report();
    if (fd < 0) {
        return 1;
    }
    if (send_command(fd, cmd) != 0) {
        close(fd);
        return 1;
    }

    char buf[REPLY_MAX];
    for (;;) {
        const ssize_t got = read(fd, buf, sizeof(buf));
        if (got > 0) {
            fwrite(buf, 1, (size_t)got, stdout);
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            fprintf(stderr, "linsight-cli: read failed: %s\n", strerror(errno));
        }
        break;
    }
    close(fd);
    return 0;
}

/* CONTROL command: print OK / BAD_ARG / FAILED from the device ACK. */
static int ctl_cmd_ack(const char *cmd)
{
    const int fd = connect_or_report();
    if (fd < 0) {
        return 1;
    }
    if (send_command(fd, cmd) != 0) {
        close(fd);
        return 1;
    }

    char buf[REPLY_MAX];
    size_t used = 0;
    for (;;) {
        const ssize_t got = read(fd, buf + used, sizeof(buf) - 1 - used);
        if (got > 0) {
            used += (size_t)got;
            if (used >= sizeof(buf) - 1) {
                break;
            }
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    buf[used] = '\0';
    close(fd);

    const char *code = strstr(buf, "\"code\":");
    if (!code) {
        if (used) {
            fputs(buf, stderr);
        } else {
            fprintf(stderr, "linsight-cli: no reply from daemon\n");
        }
        return 1;
    }
    switch (atoi(code + strlen("\"code\":"))) {
    case 0:
        puts("OK");
        return 0;
    case 2:
        puts("BAD_ARG");
        return 2;
    case 3:
        puts("FAILED");
        return 3;
    default:
        printf("ACK %s", buf);
        return 0;
    }
}

static bool parse_u32(const char *s, uint32_t max, uint32_t *out)
{
    char *end = NULL;
    errno = 0;
    const unsigned long v = strtoul(s, &end, 0);
    if (errno || end == s || *end != '\0' || v > max) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    errno = 0;
    const unsigned long long v = strtoull(s, &end, 10);
    if (errno || end == s || *end != '\0') {
        return false;
    }
    *out = (uint64_t)v;
    return true;
}

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int cmd_stats(int argc, char **argv)
{
    bool watch = false;
    int interval_ms = 1000;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--watch") == 0) {
            watch = true;
        } else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            char *end = NULL;
            const long v = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || v < 100 || v > 60000) {
                fprintf(stderr, "linsight-cli: --interval must be 100..60000 ms\n");
                return 1;
            }
            interval_ms = (int)v;
        } else {
            usage();
            return 1;
        }
    }

    if (!watch) {
        return ctl_cmd("stats");
    }

    struct sigaction sa = {.sa_handler = on_sigint};
    sigaction(SIGINT, &sa, NULL);
    while (!g_stop) {
        if (ctl_cmd("stats") != 0) {
            return 1;
        }
        fflush(stdout);
        struct timespec ts = {
            .tv_sec = interval_ms / 1000,
            .tv_nsec = (long)(interval_ms % 1000) * 1000000L,
        };
        nanosleep(&ts, NULL);
    }
    return 0;
}

/* Pick a stable ANSI colour from the candump line's CAN id. */
static unsigned line_id(const char *line)
{
    const char *p = strstr(line, "can0");
    if (!p) {
        return 0;
    }
    p += 4;
    while (*p == ' ') {
        p++;
    }
    return (unsigned)strtoul(p, NULL, 16);
}

static int cmd_live(int argc, char **argv)
{
    bool color = false;
    bool id_valid = false;
    uint32_t id = 0;
    int ext = -1;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--color") == 0) {
            color = true;
        } else if (strcmp(argv[i], "--ext") == 0 || strcmp(argv[i], "--std") == 0) {
            const int want = argv[i][2] == 'e' ? 1 : 0;
            if (ext >= 0 && ext != want) {
                fprintf(stderr, "linsight-cli: --ext and --std are mutually exclusive\n");
                return 1;
            }
            ext = want;
        } else if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], 0x1FFFFFFFu, &id)) {
                fprintf(stderr, "linsight-cli: bad --id '%s'\n", argv[i]);
                return 1;
            }
            id_valid = true;
        } else {
            usage();
            return 1;
        }
    }

    char cmd[80];
    int n = snprintf(cmd, sizeof(cmd), "live");
    if (id_valid) {
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " id=0x%X", id);
    }
    if (ext == 1) {
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " ext");
    } else if (ext == 0) {
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " std");
    }
    if (n <= 0 || n >= (int)sizeof(cmd)) {
        return 1;
    }

    const int fd = connect_or_report();
    if (fd < 0) {
        return 1;
    }
    if (send_command(fd, cmd) != 0) {
        close(fd);
        return 1;
    }

    FILE *in = fdopen(fd, "r");
    if (!in) {
        perror("fdopen");
        close(fd);
        return 1;
    }
    const bool use_color = color && isatty(STDOUT_FILENO);
    char line[512];
    while (fgets(line, sizeof(line), in)) {
        if (use_color) {
            const size_t len = strlen(line);
            if (len && line[len - 1] == '\n') {
                line[len - 1] = '\0';
            }
            printf("\033[%dm%s\033[0m\n", 31 + (int)(line_id(line) % 6), line);
        } else {
            fputs(line, stdout);
        }
        fflush(stdout);
    }
    fclose(in); /* closes fd */
    return 0;
}

static int cmd_config(int argc, char **argv)
{
    char cmd[128];
    int n = snprintf(cmd, sizeof(cmd), "config");
    bool any = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            char *end = NULL;
            const long v = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' ||
                (v != 125000 && v != 250000 && v != 500000 && v != 1000000)) {
                fprintf(stderr,
                        "linsight-cli: --bitrate must be 125000, 250000, 500000 or 1000000\n");
                return 1;
            }
            n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " bitrate=%ld", v);
            any = true;
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcasecmp(mode, "listen") != 0 &&
                strcasecmp(mode, "listen_only") != 0 &&
                strcasecmp(mode, "normal") != 0 &&
                strcasecmp(mode, "selftest") != 0 &&
                strcasecmp(mode, "no_ack") != 0) {
                fprintf(stderr, "linsight-cli: --mode must be listen|normal|selftest\n");
                return 1;
            }
            n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " mode=%s", mode);
            any = true;
        } else {
            usage();
            return 1;
        }
        if (n <= 0 || n >= (int)sizeof(cmd)) {
            fprintf(stderr, "linsight-cli: config command too long\n");
            return 1;
        }
    }
    if (!any) {
        usage();
        return 1;
    }
    return ctl_cmd_ack(cmd);
}

static int cmd_export(int argc, char **argv)
{
    if (argc < 4) {
        usage();
        return 1;
    }
    const char *fmt = argv[2];
    const char *lsc = argv[3];
    const char *out = "-";
    lsc_filter_t filter = {.ext = -1};

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], 0x1FFFFFFFu, &filter.id)) {
                fprintf(stderr, "linsight-cli: bad --id '%s'\n", argv[i]);
                return 1;
            }
            filter.id_valid = true;
        } else if (strcmp(argv[i], "--ext") == 0) {
            if (filter.ext == 0) {
                fprintf(stderr, "linsight-cli: --ext and --std are mutually exclusive\n");
                return 1;
            }
            filter.ext = 1;
        } else if (strcmp(argv[i], "--std") == 0) {
            if (filter.ext == 1) {
                fprintf(stderr, "linsight-cli: --ext and --std are mutually exclusive\n");
                return 1;
            }
            filter.ext = 0;
        } else if (strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &filter.from_wall)) {
                fprintf(stderr, "linsight-cli: bad --from '%s'\n", argv[i]);
                return 1;
            }
            filter.from_valid = true;
        } else if (strcmp(argv[i], "--to") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &filter.to_wall)) {
                fprintf(stderr, "linsight-cli: bad --to '%s'\n", argv[i]);
                return 1;
            }
            filter.to_valid = true;
        } else {
            usage();
            return 1;
        }
    }
    if (filter.from_valid && filter.to_valid && filter.to_wall <= filter.from_wall) {
        fprintf(stderr, "linsight-cli: --to must be greater than --from\n");
        return 1;
    }

    long n;
    if (strcmp(fmt, "asc") == 0) {
        n = canlog_export_asc_filtered(lsc, out, &filter);
    } else if (strcmp(fmt, "candump") == 0) {
        n = canlog_export_candump_filtered(lsc, out, &filter);
    } else {
        fprintf(stderr, "linsight-cli: unknown format '%s' (asc|candump)\n", fmt);
        return 1;
    }
    if (n < 0) {
        fprintf(stderr, "linsight-cli: export failed: %s\n",
                errno ? strerror(errno) : "bad .lsc file");
        return 1;
    }
    if (strcmp(out, "-") != 0) {
        printf("%s: %ld frames -> %s\n", fmt, n, out);
    }
    return 0;
}

static int cmd_replay(int argc, char **argv)
{
    if (argc < 3) {
        usage();
        return 1;
    }
    const char *lsc = argv[2];
    const char *iface = "can0";
    double speed = 1.0;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--iface") == 0 && i + 1 < argc) {
            iface = argv[++i];
        } else if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            speed = strtod(argv[++i], &end);
            if (errno || end == argv[i] || *end != '\0' || speed <= 0.0 || speed > 100.0) {
                fprintf(stderr, "linsight-cli: --speed must be 0.01..100\n");
                return 1;
            }
        } else {
            usage();
            return 1;
        }
    }

    replay_stats_t st;
    if (canlog_replay(lsc, iface, speed, &st) != 0) {
        fprintf(stderr, "linsight-cli: replay failed: %s\n", strerror(errno));
        return 1;
    }
    printf("replay: %llu frames on %s in %.3f s (%.2fx), late p99 %.2f ms, max %.2f ms\n",
           (unsigned long long)st.frames, iface, (double)st.elapsed_ns / 1e9, speed,
           (double)st.late_p99_ns / 1e6, (double)st.late_max_ns / 1e6);
    return 0;
}

static int cmd_compare(int argc, char **argv)
{
    if (argc != 4) {
        usage();
        return 1;
    }

    compare_stats_t st;
    if (canlog_compare(argv[2], argv[3], &st) != 0) {
        fprintf(stderr, "linsight-cli: compare failed: %s\n", strerror(errno));
        return 1;
    }
    printf("compare: ref %llu, cand %llu, matched %llu, missing %llu, "
           "extra %llu, out-of-order %llu\n",
           (unsigned long long)st.ref_frames, (unsigned long long)st.cand_frames,
           (unsigned long long)st.matched, (unsigned long long)st.missing,
           (unsigned long long)st.extra, (unsigned long long)st.order);
    return (st.missing || st.extra || st.order) ? 1 : 0;
}

/* Move --ctl out of argv so subcommands only see their own options. */
static int strip_global_args(int argc, char **argv)
{
    int out = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ctl") == 0 && i + 1 < argc) {
            g_ctl = argv[++i];
        } else {
            argv[out++] = argv[i];
        }
    }
    return out;
}

int main(int argc, char **argv)
{
    argc = strip_global_args(argc, argv);
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *cmd = argv[1];
    if (strcmp(cmd, "stats") == 0) {
        return cmd_stats(argc, argv);
    }
    if (strcmp(cmd, "live") == 0) {
        return cmd_live(argc, argv);
    }
    if (strcmp(cmd, "config") == 0) {
        return cmd_config(argc, argv);
    }
    if (strcmp(cmd, "export") == 0) {
        return cmd_export(argc, argv);
    }
    if (strcmp(cmd, "replay") == 0) {
        return cmd_replay(argc, argv);
    }
    if (strcmp(cmd, "compare") == 0) {
        return cmd_compare(argc, argv);
    }
    if (strcmp(cmd, "pause") == 0 || strcmp(cmd, "resume") == 0 ||
        strcmp(cmd, "clear") == 0) {
        return ctl_cmd_ack(cmd);
    }

    fprintf(stderr, "linsight-cli: unknown command '%s'\n", cmd);
    usage();
    return 1;
}
