#define _DEFAULT_SOURCE

#include "vcan.h"

#include <errno.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "util.h"

#define TAG "linsightd"

static int g_fd = -1;
static uint64_t g_written;
static uint64_t g_dropped;

int vcan_open(const char *ifname)
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
    g_fd = fd;
    return fd;
}

void vcan_close(void)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

bool vcan_is_open(void)
{
    return g_fd >= 0;
}

void vcan_forward(const linsight_can_record_t *recs, uint32_t n)
{
    if (g_fd < 0) {
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

        if (write(g_fd, &cf, sizeof(cf)) == (ssize_t)sizeof(cf)) {
            g_written++;
        } else {
            g_dropped++;
        }
    }
}

void vcan_stats(uint64_t *written, uint64_t *dropped)
{
    if (written) {
        *written = g_written;
    }
    if (dropped) {
        *dropped = g_dropped;
    }
}
