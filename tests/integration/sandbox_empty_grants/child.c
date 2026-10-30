/* Child for sandbox_empty_grants: try a file read, a TCP connect and an exec,
 * and report which of them were NOT refused through the exit status.
 *   bit 0 (1)  open(argv[1], O_RDONLY) succeeded
 *   bit 1 (2)  connect() to 127.0.0.1 got past the sandbox (any errno other
 *              than EACCES/EPERM; nothing listens, so ECONNREFUSED is "allowed")
 *   bit 6 (64) execve of /bin/sh happened (the exec'd shell exits with the
 *              status, so a refused exec leaves the bit clear)
 * Exit 0 therefore means every attempt was refused. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

extern char** environ;

int main(int argc, char** argv) {
    int bits = 0;
    if (argc < 2) return 99;

    int fd = open(argv[1], O_RDONLY);
    if (fd >= 0) { bits |= 1; close(fd); }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0) {
        struct sockaddr_in a = { 0 };
        a.sin_family = AF_INET;
        a.sin_port = htons(1);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, (struct sockaddr*)&a, sizeof a) == 0 ||
            (errno != EACCES && errno != EPERM)) bits |= 2;
        close(s);
    }

    char cmd[32];
    snprintf(cmd, sizeof cmd, "exit %d", bits | 64);
    char* sh_argv[] = { (char*)"sh", (char*)"-c", cmd, NULL };
    /* execve, not execv: glibc's execv calls its internal __execve, which an
     * LD_PRELOAD override of execve does not see. */
    execve("/bin/sh", sh_argv, environ);   /* returns only if refused */
    return bits;
}
