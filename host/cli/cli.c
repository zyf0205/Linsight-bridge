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
 *   linsight-cli decode FILE.lsc --dbc MODEL.dbc [--id ID] [--limit N]
 *
 * All commands use the daemon's control socket; `export`, `replay`, `compare`
 * and `decode` read files directly and therefore work without a running daemon.
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "canlog.h"
#include "compare.h"
#include "dbc.h"
#include "import.h"
#include "replay.h"

#define DEFAULT_CTL "/tmp/linsight.sock"
#define SYSTEM_CTL "/run/linsight/ctl.sock"
#define CLI_VERSION "0.3.0"
#define REPLY_MAX 4096

static const char *g_ctl;
static int g_ctl_port;
static bool g_version;
static volatile sig_atomic_t g_stop;

static void usage(void)
{
    fprintf(stderr,
            "usage: linsight-cli stats [--watch] [--interval MS]\n"
            "       linsight-cli live [--id ID] [--ext|--std] [--color]\n"
            "       linsight-cli config [--bitrate N] [--mode listen|normal|selftest]\n"
            "       linsight-cli pause [--lease MS] | resume | clear\n"
            "       linsight-cli sim start engine|bms|mix|counter [RATE] | stop | status\n"
            "       linsight-cli sim inject ID [ext] [rtr] [HEXBYTES]\n"
            "       linsight-cli export asc|candump FILE.lsc [-o OUT]\n"
            "                         [--id ID] [--ext|--std] [--from EPOCH] [--to EPOCH]\n"
            "       linsight-cli replay FILE.lsc [--iface can0] [--speed 1.0]\n"
            "       linsight-cli compare REF.lsc CAND.lsc\n"
            "       linsight-cli decode FILE.lsc --dbc MODEL.dbc [--id ID] [--limit N]\n"
            "       linsight-cli import FILE.lsc -o OUT.lsc [--boot-wall EPOCH]\n"
            "       linsight-cli verify FILE.lsc [--strict]\n"
            "       global: --ctl PATH | --ctl-port PORT | --version\n"
            "       (default " SYSTEM_CTL ", " DEFAULT_CTL ", or $LINSIGHT_CTL)\n");
}

static int ctl_connect(void)
{
    int fd;
    if (g_ctl_port > 0) {
        fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            return -1;
        }
        struct sockaddr_in addr = {.sin_family = AF_INET};
        addr.sin_port = htons((uint16_t)g_ctl_port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }
    } else {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        struct sockaddr_un addr = {.sun_family = AF_UNIX};
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_ctl);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }
    }

    /* A daemon with no device must not leave a control command blocked. */
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static int connect_or_report(void)
{
    const int fd = ctl_connect();
    if (fd < 0) {
        if (g_ctl_port > 0) {
            fprintf(stderr, "linsight-cli: cannot connect to 127.0.0.1:%d: %s\n",
                    g_ctl_port, strerror(errno));
        } else {
            fprintf(stderr, "linsight-cli: cannot connect to %s: %s\n", g_ctl,
                    strerror(errno));
        }
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
    int rc = 0;
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
            rc = 1;
        }
        break;
    }
    close(fd);
    return rc;
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
        /* 4 = the daemon never saw an ACK from the device (or timed out). */
        return strstr(buf, "ack timeout") ? 4 : 1;
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

/* Simulator control: sim start/stop/status/inject ride the same control
 * socket; the daemon replies with a plain "OK ..." line (no ACK involved). */
static int cmd_sim(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: linsight-cli sim start engine|bms|mix|counter [RATE]\n"
                        "       linsight-cli sim stop\n"
                        "       linsight-cli sim status\n"
                        "       linsight-cli sim inject ID [ext] [rtr] [DLC] [HEXBYTES]\n");
        return 1;
    }

    char cmd[160];
    int n = 0;
    if (strcmp(argv[2], "start") == 0) {
        if (argc < 4) {
            fprintf(stderr, "linsight-cli: sim start needs a pattern\n");
            return 1;
        }
        const char *pattern = argv[3];
        if (strcmp(pattern, "engine") != 0 && strcmp(pattern, "bms") != 0 &&
            strcmp(pattern, "mix") != 0 && strcmp(pattern, "counter") != 0) {
            fprintf(stderr, "linsight-cli: pattern must be engine|bms|mix|counter\n");
            return 1;
        }
        unsigned rate = 50;
        if (argc > 4 && !parse_u32(argv[4], 1000u, &rate)) {
            fprintf(stderr, "linsight-cli: RATE must be 0..1000\n");
            return 1;
        }
        n = snprintf(cmd, sizeof(cmd), "sim start %s %u", pattern, rate);
    } else if (strcmp(argv[2], "stop") == 0) {
        n = snprintf(cmd, sizeof(cmd), "sim stop");
    } else if (strcmp(argv[2], "status") == 0) {
        n = snprintf(cmd, sizeof(cmd), "sim status");
    } else if (strcmp(argv[2], "inject") == 0) {
        if (argc < 4) {
            fprintf(stderr, "linsight-cli: sim inject needs an ID (hex)\n");
            return 1;
        }
        uint32_t id;
        /* Accept hex with or without 0x (base 16 either way: CAN IDs are hex
         * by convention), e.g. "1AB" or "0x1AB". */
        char idbuf[16];
        snprintf(idbuf, sizeof(idbuf), "%s", argv[3]);
        if (strncasecmp(idbuf, "0x", 2) == 0) {
            memmove(idbuf, idbuf + 2, strlen(idbuf + 2) + 1);
        }
        char *end = NULL;
        errno = 0;
        const unsigned long v = strtoul(idbuf, &end, 16);
        if (errno || end == idbuf || *end != '\0' || v > 0x1FFFFFFFUL) {
            fprintf(stderr, "linsight-cli: bad ID '%s' (hex, max 1FFFFFFF)\n", argv[3]);
            return 1;
        }
        id = (uint32_t)v;
        n = snprintf(cmd, sizeof(cmd), "sim inject %X", id);
        for (int i = 4; i < argc && n > 0 && n < (int)sizeof(cmd); i++) {
            n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " %s", argv[i]);
        }
    } else {
        fprintf(stderr, "linsight-cli: unknown sim subcommand '%s'\n", argv[2]);
        return 1;
    }
    if (n <= 0 || n >= (int)sizeof(cmd)) {
        fprintf(stderr, "linsight-cli: sim command too long\n");
        return 1;
    }
    return ctl_cmd(cmd);
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

static int cmd_decode(int argc, char **argv)
{
    if (argc < 3) {
        usage();
        return 1;
    }
    const char *lsc = argv[2];
    const char *dbc_path = NULL;
    long limit = 100; /* 0 = unlimited */
    bool id_valid = false;
    uint32_t want_id = 0;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--dbc") == 0 && i + 1 < argc) {
            dbc_path = argv[++i];
        } else if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], 0x1FFFFFFFu, &want_id)) {
                fprintf(stderr, "linsight-cli: bad --id '%s'\n", argv[i]);
                return 1;
            }
            id_valid = true;
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            char *end = NULL;
            const long v = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || v < 0) {
                fprintf(stderr, "linsight-cli: bad --limit '%s'\n", argv[i]);
                return 1;
            }
            limit = v;
        } else {
            usage();
            return 1;
        }
    }
    if (!dbc_path) {
        fprintf(stderr, "linsight-cli: decode requires --dbc MODEL.dbc\n");
        return 1;
    }

    dbc_t *model = calloc(1, sizeof(*model));
    if (!model) {
        fprintf(stderr, "linsight-cli: out of memory\n");
        return 1;
    }
    if (dbc_load(model, dbc_path) != 0) {
        fprintf(stderr, "linsight-cli: cannot read %s: %s\n", dbc_path, strerror(errno));
        free(model);
        return 1;
    }

    lsc_reader_t r;
    if (lsc_reader_open(&r, lsc) != 0) {
        fprintf(stderr, "linsight-cli: cannot read %s: %s\n", lsc,
                errno ? strerror(errno) : "bad .lsc file");
        free(model);
        return 1;
    }

    linsight_can_record_t rec;
    long total = 0, decoded = 0, unknown = 0, shown = 0;
    int rc;
    while ((rc = lsc_reader_next(&r, &rec)) == 1) {
        total++;
        if (id_valid && rec.id != want_id) {
            continue;
        }
        const int ext = (rec.flags & LINSIGHT_REC_FLAG_EXT) ? 1 : 0;
        const dbc_message_t *m = dbc_find(model, rec.id, ext);
        if (!m) {
            unknown++;
            continue;
        }
        decoded++;
        if (limit > 0 && shown >= limit) {
            continue;
        }
        printf("0x%X [%s]", rec.id, m->name);
        for (size_t s = 0; s < m->signal_count; s++) {
            const dbc_signal_t *sig = &m->signals[s];
            double value = 0;
            if (dbc_decode_signal(sig, rec.data, rec.dlc, &value) == 0) {
                printf(" %s=%.10g%s%s", sig->name, value, sig->unit[0] ? " " : "",
                       sig->unit);
            } else {
                printf(" %s=--", sig->name);
            }
        }
        printf("\n");
        shown++;
    }
    lsc_reader_close(&r);
    if (rc < 0) {
        fprintf(stderr, "linsight-cli: read error in %s\n", lsc);
    }
    printf("decode: %ld frames, %ld decoded, %ld unknown, %ld shown\n",
           total, decoded, unknown, shown);
    free(model);
    return 0;
}

static int cmd_import(int argc, char **argv)
{
    if (argc < 3) {
        usage();
        return 1;
    }
    const char *src = argv[2];
    const char *dst = NULL;
    bool have_boot = false;
    uint64_t boot_wall = 0;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            dst = argv[++i];
        } else if (strcmp(argv[i], "--boot-wall") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &boot_wall)) {
                fprintf(stderr, "linsight-cli: bad --boot-wall '%s'\n", argv[i]);
                return 1;
            }
            have_boot = true;
        } else {
            usage();
            return 1;
        }
    }
    if (!dst) {
        fprintf(stderr, "linsight-cli: import needs -o OUT.lsc\n");
        return 1;
    }

    lsc_import_info_t info;
    char err[256] = {0};
    if (lsc_import(src, dst, have_boot, boot_wall, &info, err, sizeof(err)) != 0) {
        fprintf(stderr, "linsight-cli: import failed: %s\n", err);
        return 1;
    }
    printf("import: %llu records, boot %u, bitrate %u, anchor %llu ms (%s)\n",
           (unsigned long long)info.records, (unsigned)info.boot_count,
           (unsigned)info.bitrate, (unsigned long long)info.start_wall_ms,
           info.uptime_only ? "re-anchored" : "wall clock preserved");
    if (info.uptime_only && !have_boot) {
        fprintf(stderr, "linsight-cli: warning: anchored from file mtime; "
                        "use --boot-wall for exact dates\n");
    }
    lsc_print_sidecars(src, stdout);
    return 0;
}

static int cmd_verify(int argc, char **argv)
{
    if (argc < 3) {
        usage();
        return 1;
    }
    const char *path = argv[2];
    bool strict = false;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--strict") == 0) {
            strict = true;
        } else {
            usage();
            return 1;
        }
    }

    lsc_import_info_t info;
    char err[256] = {0};
    const int rc = lsc_verify(path, strict, &info, err, sizeof(err));
    printf("verify: records=%llu boot=%u bitrate=%u flags=0x%X uptime_only=%d "
           "gaps=%llu missing=%llu backwards=%llu dup=%llu ambiguous=%llu "
           "truncated=%d\n",
           (unsigned long long)info.records, (unsigned)info.boot_count,
           (unsigned)info.bitrate, (unsigned)info.flags, info.uptime_only ? 1 : 0,
           (unsigned long long)info.gaps, (unsigned long long)info.missing,
           (unsigned long long)info.backwards, (unsigned long long)info.duplicates,
           (unsigned long long)info.ambiguous, info.truncated ? 1 : 0);
    if (rc != 0) {
        fprintf(stderr, "linsight-cli: verify: %s\n", err);
    }
    return rc;
}

static int cmd_pause(int argc, char **argv)
{
    uint32_t lease = 0;
    bool has_lease = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--lease") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], 1800000u, &lease) || lease < 5000) {
                fprintf(stderr, "linsight-cli: --lease must be 5000..1800000 ms\n");
                return 1;
            }
            has_lease = true;
        } else {
            usage();
            return 1;
        }
    }
    char cmd[64];
    const int n = has_lease ? snprintf(cmd, sizeof(cmd), "pause lease=%u", lease)
                            : snprintf(cmd, sizeof(cmd), "pause");
    if (n <= 0 || n >= (int)sizeof(cmd)) {
        return 1;
    }
    return ctl_cmd_ack(cmd);
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

/* Move global options out of argv so subcommands only see their own. */
static int strip_global_args(int argc, char **argv)
{
    int out = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ctl") == 0 && i + 1 < argc) {
            g_ctl = argv[++i];
        } else if (strcmp(argv[i], "--ctl-port") == 0 && i + 1 < argc) {
            char *end = NULL;
            const long v = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || v <= 0 || v > 65535) {
                fprintf(stderr, "linsight-cli: bad --ctl-port '%s'\n", argv[i]);
                return -1;
            }
            g_ctl_port = (int)v;
        } else if (strcmp(argv[i], "--version") == 0) {
            g_version = true;
        } else {
            argv[out++] = argv[i];
        }
    }
    return out;
}

int main(int argc, char **argv)
{
    argc = strip_global_args(argc, argv);
    if (argc < 0) {
        return 1;
    }
    if (g_version) {
        puts("linsight-cli " CLI_VERSION);
        return 0;
    }
    if (!g_ctl) {
        const char *env = getenv("LINSIGHT_CTL");
        if (env && *env) {
            g_ctl = env;
        } else if (access(SYSTEM_CTL, F_OK) == 0) {
            g_ctl = SYSTEM_CTL;
        } else {
            g_ctl = DEFAULT_CTL;
        }
    }
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
    if (strcmp(cmd, "decode") == 0) {
        return cmd_decode(argc, argv);
    }
    if (strcmp(cmd, "import") == 0) {
        return cmd_import(argc, argv);
    }
    if (strcmp(cmd, "verify") == 0) {
        return cmd_verify(argc, argv);
    }
    if (strcmp(cmd, "pause") == 0) {
        return cmd_pause(argc, argv);
    }
    if (strcmp(cmd, "sim") == 0) {
        return cmd_sim(argc, argv);
    }
    if (strcmp(cmd, "resume") == 0 || strcmp(cmd, "clear") == 0) {
        return ctl_cmd_ack(cmd);
    }

    fprintf(stderr, "linsight-cli: unknown command '%s'\n", cmd);
    usage();
    return 1;
}
