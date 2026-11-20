/* Exec argv[1] with SIGCHLD ignored, as the Erlang VM starts its children:
 * an ignored disposition is kept across exec. */
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    signal(SIGCHLD, SIG_IGN);
    execv(argv[1], argv + 1);
    perror("execv");
    return 2;
}
