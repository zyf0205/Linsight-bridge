#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "frame.h"

/*
 * Minimal HTTP/1.1 server for the live dashboard.
 *
 * It is part of the daemon's single epoll loop: no threads, no external
 * libraries. GET only:
 *   /                 static web/index.html
 *   /api/stats        daemon stats JSON (same payload as `linsight-cli stats`)
 *   /api/live         Server-Sent Events, one JSON array per CAN batch
 *   /api/control      ?cmd=pause|resume|clear|config%20bitrate=...%20mode=...
 *
 * Live clients get no outbound queue: when the socket buffer is full an event
 * is dropped (same policy as the CLI `live` stream) so a slow browser can
 * never stall device RX or the .lsc writer.
 */

#define HTTP_TAG_LISTEN 0x1000u
#define HTTP_TAG_CLIENT_BASE 0x1001u
#define HTTP_MAX_CLIENTS 8

typedef struct http_server http_server_t;

/* Build the stats JSON body; returns 0 on failure. */
typedef size_t (*http_stats_cb)(void *ctx, char *buf, size_t cap);
/* Execute a whitelisted control command. Returns false and fills err on
 * rejection/failure. Must not block. */
typedef bool (*http_control_cb)(void *ctx, const char *cmd, char *err, size_t errcap);

/* Returns NULL when the listener cannot be created. The web page may still be
 * missing; the server then answers 503 with a hint instead of failing. */
http_server_t *http_server_new(int epoll_fd, const char *bind_addr, uint16_t port,
                               const char *web_dir, http_stats_cb stats_cb,
                               http_control_cb control_cb, void *ctx);
void http_server_free(http_server_t *s);

void http_server_accept(http_server_t *s);
void http_server_handle(http_server_t *s, int slot, uint32_t events);
void http_broadcast_records(http_server_t *s, const linsight_can_record_t *recs, uint32_t n);
