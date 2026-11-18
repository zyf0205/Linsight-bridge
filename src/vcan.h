#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "frame.h"

int vcan_open(const char *ifname);
void vcan_close(void);
bool vcan_is_open(void);
void vcan_forward(const linsight_can_record_t *recs, uint32_t n);
void vcan_stats(uint64_t *written, uint64_t *dropped);
