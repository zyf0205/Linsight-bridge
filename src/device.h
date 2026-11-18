#pragma once

#include <stdint.h>

void device_init(int epoll_fd);
void device_accept(int listen_fd);
void device_event(uint32_t events);
void device_check_idle(void);
uint64_t device_records(void);
void device_close(void);
