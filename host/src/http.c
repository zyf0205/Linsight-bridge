#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define HTTP_IN_CAP 2048
#define HTTP_OUT_CAP 32768
#define HTTP_SSE_CHUNK 64u
#define HTTP_EVENT_CAP 8192u
#define HTTP_PAGE_MAX (256u * 1024u)

typedef enum {
    HC_FREE = 0,
    HC_REQUEST,
    HC_SSE,
} http_conn_state_t;

typedef struct {
    int fd;
    http_conn_state_t state;
    bool close_after;
    char in[HTTP_IN_CAP];
    size_t in_len;
    char out[HTTP_OUT_CAP];
    size_t out_len;
    size_t out_off;
} http_conn_t;

struct http_server {
    int epoll_fd;
    int listen_fd;
    http_conn_t conns[HTTP_MAX_CLIENTS];
    size_t sse_count;
    char *page;
    size_t page_len;
    http_stats_cb stats_cb;
    http_control_cb control_cb;
    void *ctx;
};

static void set_nonblock(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

static void conn_set_events(http_server_t *s, http_conn_t *c, bool want_write)
{
    struct epoll_event ev = {
        .events = (uint32_t)(EPOLLIN | (want_write ? EPOLLOUT : 0u)),
        .data.u64 = HTTP_TAG_CLIENT_BASE + (uint64_t)(c - s->conns),
    };
    epoll_ctl(s->epoll_fd, EPOLL_CTL_MOD, c->fd, &ev);
}

static void conn_close(http_server_t *s, http_conn_t *c)
{
    if (c->fd < 0) {
        return;
    }
    epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    if (c->state == HC_SSE && s->sse_count > 0) {
        s->sse_count--;
    }
    c->fd = -1;
    c->state = HC_FREE;
    c->close_after = false;
    c->in_len = 0;
    c->out_len = 0;
    c->out_off = 0;
}

/* Drain the out buffer. Returns false when the connection was closed. */
static bool conn_flush(http_server_t *s, http_conn_t *c)
{
    while (c->out_off < c->out_len) {
        const ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off,
                               MSG_NOSIGNAL);
        if (n > 0) {
            c->out_off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true; /* keep EPOLLOUT registered */
        }
        conn_close(s, c);
        return false;
    }

    c->out_len = 0;
    c->out_off = 0;
    if (c->close_after) {
        conn_close(s, c);
        return false;
    }
    conn_set_events(s, c, false);
    return true;
}

static void http_reply(http_server_t *s, http_conn_t *c, int code, const char *reason,
                       const char *ctype, const char *body, size_t body_len)
{
    char hdr[256];
    const int n = snprintf(hdr, sizeof(hdr),
                           "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                           "Content-Length: %zu\r\nCache-Control: no-store\r\n"
                           "Connection: close\r\n\r\n",
                           code, reason, ctype, body_len);
    if (n <= 0 || (size_t)n >= sizeof(hdr) ||
        c->out_len + (size_t)n + body_len > sizeof(c->out)) {
        conn_close(s, c);
        return;
    }

    memcpy(c->out + c->out_len, hdr, (size_t)n);
    c->out_len += (size_t)n;
    if (body_len) {
        memcpy(c->out + c->out_len, body, body_len);
        c->out_len += body_len;
    }
    c->close_after = true;
    conn_set_events(s, c, true);
    conn_flush(s, c);
}

static void http_error(http_server_t *s, http_conn_t *c, int code, const char *reason,
                       const char *text)
{
    http_reply(s, c, code, reason, "text/plain; charset=utf-8", text, strlen(text));
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Extract key=value from a URL query, percent-decoding the value. */
static bool query_token(const char *query, const char *key, char *out, size_t cap)
{
    if (!query) {
        return false;
    }
    const size_t klen = strlen(key);
    while (*query) {
        const char *next = strchr(query, '&');
        const size_t len = next ? (size_t)(next - query) : strlen(query);
        if (len > klen && strncmp(query, key, klen) == 0 && query[klen] == '=') {
            const char *v = query + klen + 1;
            const size_t vlen = len - klen - 1;
            size_t o = 0;
            for (size_t i = 0; i < vlen && o + 1 < cap; i++) {
                char ch = v[i];
                if (ch == '+') {
                    ch = ' ';
                } else if (ch == '%') {
                    if (i + 2 >= vlen) {
                        return false;
                    }
                    const int hi = hexval(v[i + 1]);
                    const int lo = hexval(v[i + 2]);
                    if (hi < 0 || lo < 0) {
                        return false;
                    }
                    ch = (char)((hi << 4) | lo);
                    i += 2;
                }
                if ((unsigned char)ch < 0x20 || ch == 0x7f) {
                    return false;
                }
                out[o++] = ch;
            }
            out[o] = '\0';
            return o > 0;
        }
        if (!next) {
            break;
        }
        query = next + 1;
    }
    return false;
}

static void sse_start(http_server_t *s, http_conn_t *c)
{
    static const char hdr[] = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/event-stream\r\n"
                              "Cache-Control: no-store\r\n"
                              "Connection: keep-alive\r\n"
                              "X-Accel-Buffering: no\r\n\r\n";
    const size_t len = sizeof(hdr) - 1;
    const ssize_t n = send(c->fd, hdr, len, MSG_NOSIGNAL);
    if (n != (ssize_t)len) {
        conn_close(s, c);
        return;
    }
    c->state = HC_SSE;
    c->in_len = 0;
    s->sse_count++;
    /* Comment frame: EventSource ignores it, `curl -N` shows the stream. */
    (void)send(c->fd, ": linsight\n\n", strlen(": linsight\n\n"),
               MSG_NOSIGNAL | MSG_DONTWAIT);
    conn_set_events(s, c, false);
}

static void process_request(http_server_t *s, http_conn_t *c, char *req)
{
    char method[8] = {0};
    char target[512] = {0};
    if (sscanf(req, "%7s %511s", method, target) != 2) {
        http_error(s, c, 400, "Bad Request", "bad request line\n");
        return;
    }
    if (strcmp(method, "GET") != 0) {
        http_error(s, c, 405, "Method Not Allowed", "GET only\n");
        return;
    }

    char *query = strchr(target, '?');
    if (query) {
        *query++ = '\0';
    }

    if (strcmp(target, "/") == 0 || strcmp(target, "/index.html") == 0) {
        if (!s->page) {
            http_error(s, c, 503, "Service Unavailable",
                       "web/index.html not found; run from host/ or pass --web-dir\n");
        } else {
            http_reply(s, c, 200, "OK", "text/html; charset=utf-8", s->page,
                       s->page_len);
        }
    } else if (strcmp(target, "/api/stats") == 0) {
        char buf[2048];
        const size_t n = s->stats_cb ? s->stats_cb(s->ctx, buf, sizeof(buf)) : 0;
        if (n == 0) {
            http_error(s, c, 503, "Service Unavailable", "stats unavailable\n");
        } else {
            http_reply(s, c, 200, "OK", "application/json", buf, n);
        }
    } else if (strcmp(target, "/api/live") == 0) {
        sse_start(s, c);
    } else if (strcmp(target, "/api/control") == 0) {
        char cmd[160];
        if (!query_token(query, "cmd", cmd, sizeof(cmd))) {
            http_error(s, c, 400, "Bad Request", "missing cmd\n");
            return;
        }
        char err[128] = {0};
        const bool ok = s->control_cb && s->control_cb(s->ctx, cmd, err, sizeof(err));
        char body[256];
        const int n = ok ? snprintf(body, sizeof(body), "{\"ok\":true}\n")
                         : snprintf(body, sizeof(body),
                                    "{\"ok\":false,\"error\":\"%s\"}\n",
                                    err[0] ? err : "rejected");
        if (n > 0) {
            http_reply(s, c, 200, "OK", "application/json", body, (size_t)n);
        }
    } else if (strcmp(target, "/favicon.ico") == 0) {
        http_reply(s, c, 204, "No Content", "text/plain", NULL, 0);
    } else {
        http_error(s, c, 404, "Not Found", "not found\n");
    }
}

static void conn_read(http_server_t *s, http_conn_t *c)
{
    char buf[1024];
    const ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
    if (n == 0) {
        conn_close(s, c);
        return;
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            conn_close(s, c);
        }
        return;
    }
    if (c->state == HC_SSE || c->close_after) {
        return; /* live clients have nothing to say; response already queued */
    }
    if (c->in_len + (size_t)n >= sizeof(c->in)) {
        http_error(s, c, 431, "Request Header Fields Too Large", "request too large\n");
        return;
    }
    memcpy(c->in + c->in_len, buf, (size_t)n);
    c->in_len += (size_t)n;
    c->in[c->in_len] = '\0';

    char *end = strstr(c->in, "\r\n\r\n");
    if (!end) {
        return; /* headers not complete yet */
    }
    *end = '\0';
    process_request(s, c, c->in);
    c->in_len = 0;
}

void http_server_handle(http_server_t *s, int slot, uint32_t events)
{
    if (!s || slot < 0 || slot >= HTTP_MAX_CLIENTS) {
        return;
    }
    http_conn_t *c = &s->conns[slot];
    if (c->fd < 0) {
        return;
    }

    if (events & EPOLLERR) {
        conn_close(s, c);
        return;
    }
    if (events & EPOLLIN) {
        conn_read(s, c);
    }
    if (c->fd < 0) {
        return;
    }
    if (events & EPOLLOUT) {
        conn_flush(s, c);
    }
    if (c->fd < 0) {
        return;
    }
    if (events & EPOLLHUP) {
        if (c->out_len == 0) {
            conn_close(s, c);
        } else {
            conn_flush(s, c);
        }
    }
}

void http_server_accept(http_server_t *s)
{
    if (!s) {
        return;
    }
    for (int k = 0; k < HTTP_MAX_CLIENTS; k++) {
        const int fd = accept(s->listen_fd, NULL, NULL);
        if (fd < 0) {
            break;
        }
        set_nonblock(fd);

        int slot = -1;
        for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
            if (s->conns[i].fd < 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            close(fd);
            continue;
        }

        http_conn_t *c = &s->conns[slot];
        memset(c, 0, sizeof(*c));
        c->fd = fd;
        c->state = HC_REQUEST;

        struct epoll_event ev = {
            .events = EPOLLIN,
            .data.u64 = HTTP_TAG_CLIENT_BASE + (uint64_t)slot,
        };
        if (epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            close(fd);
            c->fd = -1;
        }
    }
}

static void append_record_json(char *ev, size_t *used, const linsight_can_record_t *r,
                               bool first)
{
    static const char HEX[] = "0123456789ABCDEF";
    char hex[LINSIGHT_DLC_MAX * 2 + 1];
    size_t h = 0;
    const uint8_t dlc = r->dlc > LINSIGHT_DLC_MAX ? LINSIGHT_DLC_MAX : r->dlc;
    if (!(r->flags & LINSIGHT_REC_FLAG_RTR)) {
        for (uint8_t i = 0; i < dlc; i++) {
            hex[h++] = HEX[r->data[i] >> 4];
            hex[h++] = HEX[r->data[i] & 0x0Fu];
        }
    }
    hex[h] = '\0';

    const int n = snprintf(ev + *used, HTTP_EVENT_CAP - *used,
                           "%s{\"i\":%u,\"f\":%u,\"d\":%u,\"b\":\"%s\","
                           "\"t\":%u,\"s\":%u}",
                           first ? "" : ",", (unsigned)r->id, (unsigned)r->flags,
                           (unsigned)dlc, hex, (unsigned)r->ts_ms, (unsigned)r->seq);
    if (n > 0 && (size_t)n < HTTP_EVENT_CAP - *used) {
        *used += (size_t)n;
    }
}

void http_broadcast_records(http_server_t *s, const linsight_can_record_t *recs,
                            uint32_t n)
{
    if (!s || s->sse_count == 0 || !recs || n == 0) {
        return;
    }

    /* Static scratch: the server is only used from the daemon's epoll thread. */
    static char ev[HTTP_EVENT_CAP];
    for (uint32_t start = 0; start < n; start += HTTP_SSE_CHUNK) {
        uint32_t take = n - start;
        if (take > HTTP_SSE_CHUNK) {
            take = HTTP_SSE_CHUNK;
        }

        size_t used = 0;
        memcpy(ev, "data: [", 7);
        used = 7;
        for (uint32_t i = 0; i < take; i++) {
            append_record_json(ev, &used, &recs[start + i], i == 0);
        }
        memcpy(ev + used, "]\n\n", 3);
        used += 3;

        for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
            http_conn_t *c = &s->conns[i];
            if (c->fd < 0 || c->state != HC_SSE) {
                continue;
            }
            const ssize_t sent = send(c->fd, ev, used, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (sent < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    conn_close(s, c);
                }
                continue; /* a slow browser drops this batch, never stalls RX */
            }
            if ((size_t)sent != used) {
                conn_close(s, c); /* a partial SSE event corrupts the framing */
            }
        }
    }
}

static char *load_page(const char *web_dir, size_t *len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/index.html", web_dir);

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "http: cannot open %s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    const long sz = ftell(f);
    if (sz <= 0 || (unsigned long)sz > HTTP_PAGE_MAX) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *page = malloc((size_t)sz);
    if (!page) {
        fclose(f);
        return NULL;
    }
    if (fread(page, 1, (size_t)sz, f) != (size_t)sz) {
        free(page);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)sz;
    return page;
}

http_server_t *http_server_new(int epoll_fd, const char *bind_addr, uint16_t port,
                               const char *web_dir, http_stats_cb stats_cb,
                               http_control_cb control_cb, void *ctx)
{
    http_server_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->epoll_fd = epoll_fd;
    s->listen_fd = -1;
    s->stats_cb = stats_cb;
    s->control_cb = control_cb;
    s->ctx = ctx;
    for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
        s->conns[i].fd = -1;
    }
    s->page = load_page(web_dir, &s->page_len);

    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        fprintf(stderr, "http: socket: %s\n", strerror(errno));
        free(s);
        return NULL;
    }
    static const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {.sin_family = AF_INET};
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "http: bad bind address '%s'\n", bind_addr);
        close(fd);
        free(s);
        return NULL;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
        fprintf(stderr, "http: bind %s:%u: %s\n", bind_addr, (unsigned)port,
                strerror(errno));
        close(fd);
        free(s);
        return NULL;
    }
    set_nonblock(fd);

    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = HTTP_TAG_LISTEN};
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(fd);
        free(s);
        return NULL;
    }
    s->listen_fd = fd;
    return s;
}

void http_server_free(http_server_t *s)
{
    if (!s) {
        return;
    }
    for (int i = 0; i < HTTP_MAX_CLIENTS; i++) {
        if (s->conns[i].fd >= 0) {
            epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, s->conns[i].fd, NULL);
            close(s->conns[i].fd);
        }
    }
    if (s->listen_fd >= 0) {
        epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, s->listen_fd, NULL);
        close(s->listen_fd);
    }
    free(s->page);
    free(s);
}
