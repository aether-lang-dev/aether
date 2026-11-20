/* The TLS client of http_server_slow_clients, over OpenSSL: a handshake on a
 * connected socket, then records the shim sends as it likes, a byte at a
 * time included. The script links this when the build links OpenSSL, and
 * tls_client_none.c otherwise. */

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#endif

int slow_tls_available(void) { return 1; }

#ifdef _WIN32
/* The session-ticket drain's helpers (slow_tls_connect). */
static void set_nonblocking(int fd, int on) {
    u_long mode = on ? 1 : 0;
    ioctlsocket((SOCKET)fd, FIONBIO, &mode);
}

/* 1 when `fd` is readable within `ms`. */
static int readable_within(int fd, int ms) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET((SOCKET)fd, &rd);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return select(fd + 1, &rd, NULL, NULL, &tv) > 0;
}
#endif

static void recv_timeout_ms(int fd, int ms) {
#ifdef _WIN32
    DWORD tv = (DWORD)ms;
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

/* A TLS connection over `fd`, handshake done, or NULL. The server's
 * certificate is a throwaway one, not checked. */
void* slow_tls_connect(int fd) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return 0;
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, 0);
    SSL* ssl = SSL_new(ctx);
    SSL_CTX_free(ctx);   /* the SSL holds its own reference */
    if (!ssl) return 0;
    int rc = SSL_set_fd(ssl, fd) != 1 ? -2 : SSL_connect(ssl);
    if (rc != 1) {
        /* Why, so a round that fails here says so in the run log. */
        char why[256] = "";
        unsigned long e = ERR_peek_last_error();
        if (e) ERR_error_string_n(e, why, sizeof why);
        fprintf(stderr, "    tls client: handshake failed (rc %d, SSL_get_error %d, errno %d): %s\n",
                rc, rc == -2 ? -1 : SSL_get_error(ssl, rc), errno, why[0] ? why : "no OpenSSL error");
        ERR_clear_error();
        SSL_free(ssl);
        return 0;
    }
    /* A TLS 1.3 server sends its session tickets after the handshake; read
     * them now, so the socket turning readable later means the server
     * answered or closed. */
    char scratch[256];
#ifdef _WIN32
    /* Without blocking, for as long as more arrives within 300 ms. A
     * blocking read that ran into a receive timeout failed the connection
     * here, where the timeout is WSAETIMEDOUT and not a retry, and OpenSSL
     * 4 keeps it failed: every SSL_write after it failed with SSL_ERROR_SSL
     * and nothing queued (the UCRT64 leg; OpenSSL 3 let the write through). */
    set_nonblocking(fd, 1);
    while (readable_within(fd, 300)) {
        int n = SSL_read(ssl, scratch, (int)sizeof(scratch));
        if (n > 0) continue;
        int e = SSL_get_error(ssl, n);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) break;
    }
    set_nonblocking(fd, 0);
#else
    /* A blocking read under a 300 ms receive timeout: the timeout is EAGAIN
     * here, a retry, and the connection stays usable. The non-blocking
     * drain above, run here too, broke the macOS head rounds over TLS (the
     * server's 408 then read as nothing); this is the read every POSIX leg
     * passed with. */
    recv_timeout_ms(fd, 300);
    (void)SSL_read(ssl, scratch, (int)sizeof(scratch));
#endif
    ERR_clear_error();
    recv_timeout_ms(fd, 5000);
    return ssl;
}

/* `len` bytes as one record, sent. */
int slow_tls_write(void* t, const void* buf, int len) {
    int n = SSL_write((SSL*)t, buf, len);
    if (n <= 0) ERR_clear_error();
    return n;
}

/* The bytes of the record that carries `len` bytes of `buf`, into `out`
 * (up to `cap`), not sent: the shim sends them itself, as slowly as a round
 * needs. The socket stays the connection's read side. Returns the length,
 * or -1. */
int slow_tls_record(void* t, const void* buf, int len, unsigned char* out, int cap) {
    SSL* ssl = (SSL*)t;
    BIO* mem = BIO_new(BIO_s_mem());
    if (!mem) return -1;
    BIO* sock = SSL_get_wbio(ssl);
    if (!sock || BIO_up_ref(sock) != 1) {
        BIO_free(mem);
        return -1;
    }
    SSL_set0_wbio(ssl, mem);          /* gives back the connection's ref */
    int n = SSL_write(ssl, buf, len);
    int got = n > 0 ? BIO_read(mem, out, cap) : -1;
    if (n <= 0 || got <= 0) {
        /* Why, so a round that fails here says so in the run log (the
         * UCRT64 leg fails every TLS round here and nowhere else). */
        char why[256] = "";
        unsigned long e = ERR_peek_last_error();
        if (e) ERR_error_string_n(e, why, sizeof why);
        fprintf(stderr, "    tls client: no record (SSL_write %d, SSL_get_error %d, BIO_read %d, %s): %s\n",
                n, n <= 0 ? SSL_get_error(ssl, n) : 0, got, OpenSSL_version(OPENSSL_VERSION),
                why[0] ? why : "no OpenSSL error");
    }
    SSL_set0_wbio(ssl, sock);         /* frees mem, takes our ref */
    if (n <= 0) ERR_clear_error();
    return got;
}

/* A TLS 1.3 KeyUpdate, sent now: a record that carries no application
 * data. 0 once it is out, -1 when it could not be (not TLS 1.3). */
int slow_tls_key_update(void* t) {
    SSL* ssl = (SSL*)t;
    if (SSL_version(ssl) != TLS1_3_VERSION ||
        SSL_key_update(ssl, SSL_KEY_UPDATE_NOT_REQUESTED) != 1 ||
        SSL_do_handshake(ssl) != 1) {
        ERR_clear_error();
        return -1;
    }
    return 0;
}

/* How the last read that returned nothing ended, for slow_tls_explain. */
static int g_read_err;
static int g_read_errno;
static char g_read_why[256];

/* Up to `cap` bytes: >0 read, 0 the server closed, -1 an error. */
int slow_tls_read(void* t, char* buf, int cap) {
    errno = 0;
    int n = SSL_read((SSL*)t, buf, cap);
    if (n > 0) return n;
    int e = SSL_get_error((SSL*)t, n);
    g_read_err = e;
    g_read_errno = errno;
    g_read_why[0] = '\0';
    unsigned long q = ERR_peek_last_error();
    if (q) ERR_error_string_n(q, g_read_why, sizeof g_read_why);
    ERR_clear_error();
    return e == SSL_ERROR_ZERO_RETURN ? 0 : -1;
}

/* Why a response read over TLS came back empty: a close_notify with nothing
 * before it (SSL_get_error 6) is the server closing without an answer; 5
 * with an errno is the connection reset or cut; 1 a TLS error. */
void slow_tls_explain(void) {
    fprintf(stderr, "    tls client: the response read ended empty (SSL_get_error %d, errno %d, %s): %s\n",
            g_read_err, g_read_errno, OpenSSL_version(OPENSSL_VERSION),
            g_read_why[0] ? g_read_why : "no OpenSSL error");
}

void slow_tls_free(void* t) {
    if (t) SSL_free((SSL*)t);
}
