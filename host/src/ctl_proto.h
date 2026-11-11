#pragma once
/*
 * Host-side control plane shared by linsightd and linsight-cli.
 *
 * The CLI validates arguments locally and the daemon validates the same
 * command line again before forwarding it to the device. Keeping the
 * whitelists here is what stops the two sides from drifting.
 */
#include <stdbool.h>
#include <strings.h>

#include "frame.h"

#define LINSIGHT_HOST_VERSION "0.4.0"

static inline bool linsight_bitrate_valid(long bitrate)
{
    return bitrate == 125000 || bitrate == 250000 || bitrate == 500000 ||
           bitrate == 1000000;
}

static inline bool linsight_mode_valid(const char *mode)
{
    return strcasecmp(mode, "listen") == 0 ||
           strcasecmp(mode, "listen_only") == 0 ||
           strcasecmp(mode, "normal") == 0 ||
           strcasecmp(mode, "selftest") == 0 ||
           strcasecmp(mode, "no_ack") == 0;
}

static inline bool linsight_pause_lease_valid(unsigned long ms)
{
    return ms >= LINSIGHT_PAUSE_LEASE_MIN_MS && ms <= LINSIGHT_PAUSE_LEASE_MAX_MS;
}
