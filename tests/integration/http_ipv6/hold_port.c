/* hold_port HOST PORT: bind a TCP socket to HOST:PORT (HOST "127.0.0.1" or
 * "::1") without listening, print HELD (or BIND FAILED when something else
 * has the port), and keep it until killed.
 *
 * test_http_ipv6.sh asserts that a server on one family's loopback is not
 * reachable through the other family at the same port. Nothing kept that
 * port free on the other family: a test running beside it could be handed
 * it and answer (crypto_tls13_peer did, CI run 43124040748). Held here,
 * bound but not listening, a connect to it is refused whatever runs beside
 * the test, and no one else can take it until the assertion is done. */
#include <stdio.h>
#include <string.h>

#include "raw_socket.h"

#ifdef _WIN32
#define hold_sleep() Sleep(1000)
#else
#include <unistd.h>
#define hold_sleep() sleep(1)
#endif

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    if (raw_socket_start() != 0) return 1;
    int port = atoi(argv[2]);
    int v6 = strchr(argv[1], ':') != NULL;
    raw_sock s = socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
    int rc = -1;
    if (s != RAW_SOCK_INVALID) {
        if (v6) {
            struct sockaddr_in6 a;
            memset(&a, 0, sizeof(a));
            a.sin6_family = AF_INET6;
            a.sin6_port = htons((unsigned short)port);
            a.sin6_addr = in6addr_loopback;
            rc = bind(s, (struct sockaddr*)&a, sizeof(a));
        } else {
            struct sockaddr_in a;
            memset(&a, 0, sizeof(a));
            a.sin_family = AF_INET;
            a.sin_port = htons((unsigned short)port);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            rc = bind(s, (struct sockaddr*)&a, sizeof(a));
        }
    }
    printf(rc == 0 ? "HELD\n" : "BIND FAILED\n");
    fflush(stdout);
    if (rc != 0) return 1;
    for (;;) hold_sleep();
}
