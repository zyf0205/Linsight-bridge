#define _DEFAULT_SOURCE

#include "discovery.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "util.h"

#define TAG "linsightd"

#define DISCOVERY_PROBE "LINSIGHT?1"
#define DISCOVERY_PROBE_LEN 10u

int discovery_open(int port)
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

void discovery_handle(int fd, const char *tcp_port)
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
        const int reply_len = snprintf(reply, sizeof(reply), "LINSIGHT!1 %s\n", tcp_port);
        if (reply_len > 0 && (size_t)reply_len < sizeof(reply)) {
            sendto(fd, reply, (size_t)reply_len, 0, (struct sockaddr *)&peer, peer_len);
        }
    }
}
