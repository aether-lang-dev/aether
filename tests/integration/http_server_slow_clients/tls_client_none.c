/* The TLS client of http_server_slow_clients for a build without OpenSSL:
 * none, so the rounds that need one (a TLS head or body sent slowly) are
 * skipped. The handshake rounds need no client library and still run. */

int slow_tls_available(void) { return 0; }
void* slow_tls_connect(int fd) { (void)fd; return 0; }
int slow_tls_write(void* t, const void* buf, int len) { (void)t; (void)buf; (void)len; return -1; }
int slow_tls_record(void* t, const void* buf, int len, unsigned char* out, int cap) {
    (void)t; (void)buf; (void)len; (void)out; (void)cap;
    return -1;
}
int slow_tls_read(void* t, char* buf, int cap) { (void)t; (void)buf; (void)cap; return -1; }
void slow_tls_free(void* t) { (void)t; }
int slow_tls_key_update(void* t) { (void)t; return -1; }
void slow_tls_explain(void) {}
