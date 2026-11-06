#include "replay.h"

#include <errno.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "canlog.h"

#define LATE_BUCKET_NS 100000u /* 100 us */
#define LATE_BUCKETS 2048u     /* reports p99 up to ~204 ms */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t target_ns)
{
    struct timespec ts = {
        .tv_sec = (time_t)(target_ns / 1000000000ULL),
        .tv_nsec = (long)(target_ns % 1000000000ULL),
    };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

static int open_can(const char *iface)
{
    const int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_can addr = {
        .can_family = AF_CAN,
        .can_ifindex = ifr.ifr_ifindex,
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    /* A broken/overloaded bus must not hang the replay forever. */
    struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

static int send_record(int fd, const linsight_can_record_t *r)
{
    struct can_frame cf;
    memset(&cf, 0, sizeof(cf));
    cf.can_id = r->id;
    if (r->flags & LINSIGHT_REC_FLAG_EXT) {
        cf.can_id |= CAN_EFF_FLAG;
    }
    if (r->flags & LINSIGHT_REC_FLAG_RTR) {
        cf.can_id |= CAN_RTR_FLAG;
    }
    cf.can_dlc = r->dlc;
    memcpy(cf.data, r->data, r->dlc);
    return write(fd, &cf, sizeof(cf)) == (ssize_t)sizeof(cf) ? 0 : -1;
}

int canlog_replay(const char *lsc_path, const char *iface, double speed,
                  replay_stats_t *stats)
{
    if (!stats || speed <= 0.0) {
        errno = EINVAL;
        return -1;
    }
    memset(stats, 0, sizeof(*stats));

    lsc_reader_t rd;
    if (lsc_reader_open(&rd, lsc_path) != 0) {
        return -1;
    }

    linsight_can_record_t rec;
    int rc = lsc_reader_next(&rd, &rec);
    if (rc < 0) {
        lsc_reader_close(&rd);
        return -1;
    }
    if (rc == 0) {
        lsc_reader_close(&rd);
        return 0;
    }

    const int fd = open_can(iface);
    if (fd < 0) {
        lsc_reader_close(&rd);
        return -1;
    }

    uint32_t first_ts = rec.ts_ms;
    const uint64_t base = now_ns();
    uint32_t late_hist[LATE_BUCKETS] = {0};

    for (;;) {
        /* The recording may start at a non-zero device uptime; only deltas
         * matter. A backwards timestamp (corrupt file) sends immediately. */
        const uint64_t offset_ns =
            rec.ts_ms > first_ts ? (uint64_t)(rec.ts_ms - first_ts) * 1000000ULL : 0;
        const uint64_t target_ns = base + (uint64_t)((double)offset_ns / speed);

        sleep_until(target_ns);
        const uint64_t now = now_ns();
        const uint64_t late = now > target_ns ? now - target_ns : 0;
        const uint64_t bucket = late / LATE_BUCKET_NS;
        late_hist[bucket < LATE_BUCKETS ? bucket : LATE_BUCKETS - 1]++;
        if (late > stats->late_max_ns) {
            stats->late_max_ns = late;
        }

        if (send_record(fd, &rec) != 0) {
            const int saved = errno;
            close(fd);
            lsc_reader_close(&rd);
            errno = saved;
            return -1;
        }
        stats->frames++;

        rc = lsc_reader_next(&rd, &rec);
        if (rc <= 0) {
            break;
        }
    }

    /* p99 from the histogram: upper edge of the bucket holding the 99th
     * percentile. 100 us resolution is far below the <5 ms acceptance bar. */
    if (stats->frames) {
        const uint64_t target = (stats->frames * 99u + 99u) / 100u;
        uint64_t seen = 0;
        for (uint32_t i = 0; i < LATE_BUCKETS; i++) {
            seen += late_hist[i];
            if (seen >= target) {
                stats->late_p99_ns = (uint64_t)(i + 1) * LATE_BUCKET_NS;
                break;
            }
        }
    }
    stats->elapsed_ns = now_ns() - base;

    close(fd);
    lsc_reader_close(&rd);
    return rc < 0 ? -1 : 0;
}
