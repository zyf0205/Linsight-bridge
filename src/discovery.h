#pragma once

#define DISCOVERY_PORT_DEFAULT 8889

int discovery_open(int port);
void discovery_handle(int fd, const char *tcp_port);
