/* The TLS client of http_server_slow_clients, over OpenSSL: a handshake on a
 * connected socket, then records the shim sends as it likes, a byte at a
 * time included. The script links this when the build links OpenSSL, and
 * tls_client_none.c otherwise. */

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#endif

int slow_tls_available(void) { return 1; }

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
    if (SSL_set_fd(ssl, fd) != 1 || SSL_connect(ssl) != 1) {
        ERR_clear_error();
        SSL_free(ssl);
        return 0;
    }
    /* A TLS 1.3 server sends its session tickets after the handshake; read
     * them now, so the socket turning readable later means the server
     * answered or closed. The read times out once they are taken. */
    recv_timeout_ms(fd, 300);
    char scratch[256];
    (void)SSL_read(ssl, scratch, (int)sizeof(scratch));
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

/* Up to `cap` bytes: >0 read, 0 the server closed, -1 an error. */
int slow_tls_read(void* t, char* buf, int cap) {
    int n = SSL_read((SSL*)t, buf, cap);
    if (n > 0) return n;
    int e = SSL_get_error((SSL*)t, n);
    ERR_clear_error();
    return e == SSL_ERROR_ZERO_RETURN ? 0 : -1;
}

void slow_tls_free(void* t) {
    if (t) SSL_free((SSL*)t);
}
