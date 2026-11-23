/*
 * conn_auth_upstream.c — an HTTP/1.1 upstream that authenticates the
 * CONNECTION, not the request, for the client connection-owner test.
 *
 * NTLM and Negotiate work this way: a handshake on a connection makes every
 * later request on that same connection the authenticated user's, with no
 * credential on it. A client that hands a kept connection to a request from
 * someone else therefore sends that request as the user who logged in. This
 * server is the smallest scheme with that property, so a test can see which
 * connection carried a request and as whom:
 *
 *   GET /login   with "X-Login: <name>"   the connection becomes <name>'s: 200
 *   GET /whoami                           200 "conn=<n> user=<name>" on an
 *                                         authenticated connection, 401
 *                                         "conn=<n> anonymous" on any other
 *   GET /stats                            200 "accepted=<a> open=<o>": the
 *                                         connections accepted so far, and
 *                                         those still open apart from the
 *                                         one asking
 *   GET /quit                             exits
 *
 * <n> is the connection's ordinal, in accept order from 1, so a response
 * names the connection that carried it. Every response has a Content-Length
 * and the connection stays open, as a keep-alive server's does.
 *
 * It binds 127.0.0.1:0 and prints the port. One thread, select() over the
 * connections, so the order of events is the order the kernel reports them.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#include "raw_socket.h"

#define MAX_CONNS 128

typedef struct {
    raw_sock fd;
    int      id;
    char     user[64];
    char     buf[8192];
    size_t   have;
} Conn;

static Conn conns[MAX_CONNS];
static int  accepted = 0;

static void drop(Conn* c) {
    raw_socket_close(c->fd);
    c->fd = RAW_SOCK_INVALID;
    c->user[0] = '\0';
    c->have = 0;
}

static void reply(Conn* c, int status, const char* body) {
    const char* reason = status == 200 ? "OK" : "Unauthorized";
    char out[512];
    int n = snprintf(out, sizeof(out),
                     "HTTP/1.1 %d %s\r\nContent-Length: %d\r\nX-Conn: %d\r\n%s\r\n%s",
                     status, reason, (int)strlen(body), c->id,
                     status == 401 ? "WWW-Authenticate: Conn\r\n" : "", body);
    send(c->fd, out, n, 0);
}

/* A peer that closed while idle is seen here before /stats counts it: its
 * descriptor is readable and a peek reads nothing. */
static void reap_closed(Conn* asking) {
    for (int i = 0; i < MAX_CONNS; i++) {
        Conn* c = &conns[i];
        if (c == asking || c->fd == RAW_SOCK_INVALID) continue;
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(c->fd, &rs);
        struct timeval tv = {0, 0};
        if (select((int)c->fd + 1, &rs, NULL, NULL, &tv) == 1) {
            char b;
            if (recv(c->fd, &b, 1, MSG_PEEK) <= 0) drop(c);
        }
    }
}

static void header_value(const char* head, const char* name, char* out, size_t n) {
    out[0] = '\0';
    size_t nl = strlen(name);
    const char* p = strstr(head, "\r\n");
    while (p && p[2] != '\r') {
        p += 2;
        if (strncmp(p, name, nl) == 0 && p[nl] == ':') {
            const char* v = p + nl + 1;
            while (*v == ' ') v++;
            size_t i = 0;
            while (v[i] && v[i] != '\r' && i + 1 < n) { out[i] = v[i]; i++; }
            out[i] = '\0';
            return;
        }
        p = strstr(p, "\r\n");
    }
}

/* One complete request head in c->buf: answer it. Returns 0 to quit. */
static int serve(Conn* c, const char* head) {
    char body[256];
    if (strncmp(head, "GET /login ", 11) == 0) {
        header_value(head, "X-Login", c->user, sizeof(c->user));
        snprintf(body, sizeof(body), "conn=%d user=%s", c->id, c->user);
        reply(c, 200, body);
    } else if (strncmp(head, "GET /whoami ", 12) == 0) {
        if (c->user[0]) {
            snprintf(body, sizeof(body), "conn=%d user=%s", c->id, c->user);
            reply(c, 200, body);
        } else {
            snprintf(body, sizeof(body), "conn=%d anonymous", c->id);
            reply(c, 401, body);
        }
    } else if (strncmp(head, "GET /stats ", 11) == 0) {
        reap_closed(c);
        int open = 0;
        for (int i = 0; i < MAX_CONNS; i++)
            if (conns[i].fd != RAW_SOCK_INVALID && &conns[i] != c) open++;
        snprintf(body, sizeof(body), "accepted=%d open=%d", accepted, open);
        reply(c, 200, body);
    } else if (strncmp(head, "GET /quit ", 10) == 0) {
        reply(c, 200, "bye");
        return 0;
    } else {
        reply(c, 200, "?");
    }
    return 1;
}

int main(void) {
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    if (raw_socket_start() != 0) { perror("socket start"); return 1; }
    for (int i = 0; i < MAX_CONNS; i++) conns[i].fd = RAW_SOCK_INVALID;

    raw_sock ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == RAW_SOCK_INVALID) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(0);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(ls, (struct sockaddr*)&a, sizeof(a)) != 0) { perror("bind"); return 1; }
    if (listen(ls, 64) != 0) { perror("listen"); return 1; }
    struct sockaddr_in bound;
    socklen_t bl = sizeof(bound);
    if (getsockname(ls, (struct sockaddr*)&bound, &bl) != 0) { perror("getsockname"); return 1; }
    printf("%d\n", ntohs(bound.sin_port));
    fflush(stdout);

    for (;;) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(ls, &rs);
        int maxfd = (int)ls;
        for (int i = 0; i < MAX_CONNS; i++) {
            if (conns[i].fd == RAW_SOCK_INVALID) continue;
            FD_SET(conns[i].fd, &rs);
            if ((int)conns[i].fd > maxfd) maxfd = (int)conns[i].fd;
        }
        if (select(maxfd + 1, &rs, NULL, NULL, NULL) < 0) continue;

        /* Requests on open connections first, then new connections: a
         * close the client made before dialling again is seen before the
         * dial it preceded. */
        for (int i = 0; i < MAX_CONNS; i++) {
            Conn* c = &conns[i];
            if (c->fd == RAW_SOCK_INVALID || !FD_ISSET(c->fd, &rs)) continue;
            int n = (int)recv(c->fd, c->buf + c->have, sizeof(c->buf) - c->have - 1, 0);
            if (n <= 0) { drop(c); continue; }
            c->have += (size_t)n;
            c->buf[c->have] = '\0';
            char* end;
            while ((end = strstr(c->buf, "\r\n\r\n")) != NULL) {
                size_t used = (size_t)(end - c->buf) + 4;
                char head[8192];
                memcpy(head, c->buf, used);
                head[used] = '\0';
                memmove(c->buf, c->buf + used, c->have - used + 1);
                c->have -= used;
                if (!serve(c, head)) return 0;
            }
        }
        if (FD_ISSET(ls, &rs)) {
            raw_sock fd = accept(ls, NULL, NULL);
            if (fd == RAW_SOCK_INVALID) continue;
            int slot = -1;
            for (int i = 0; i < MAX_CONNS; i++)
                if (conns[i].fd == RAW_SOCK_INVALID) { slot = i; break; }
            if (slot < 0) { raw_socket_close(fd); continue; }
            conns[slot].fd = fd;
            conns[slot].id = ++accepted;
            conns[slot].user[0] = '\0';
            conns[slot].have = 0;
        }
    }
}
