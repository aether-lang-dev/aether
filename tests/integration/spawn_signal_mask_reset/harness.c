/* Regression: a child spawned via os.* must NOT inherit the parent's blocked
 * signal mask (or its SIG_IGN dispositions). An embedder that masks signals —
 * the JVM, .NET, Ruby, Julia all do — would otherwise spawn children deaf to
 * the very signals os.kill later sends them, so os.kill(token, SIGTERM) is
 * delivered but never acted on and an unbounded os.wait hangs forever.
 * asks/spawn-child-inherits-blocked-signal-mask.md.
 *
 * This blocks SIGTERM/INT/QUIT in the parent (the embedder scenario), then
 * drives the REAL spawn path (os_run_capture_raw) on a probe that reports its
 * own /proc/self/status SigBlk. The child's mask must be all-zero. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>

extern char* os_run_capture_raw(const char* prog, void* argv_list, void* env_list);

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <probe-script>\n", argv[0]); return 2; }

    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGQUIT);
    sigprocmask(SIG_BLOCK, &block, NULL);

    /* Sanity: confirm WE really have them blocked, so a pass means the child
     * diverged from the parent (the reset worked), not that nothing was set. */
    sigset_t cur;
    sigprocmask(SIG_BLOCK, NULL, &cur);
    if (!sigismember(&cur, SIGTERM)) {
        printf("FAIL: parent could not block SIGTERM (test setup)\n");
        return 1;
    }

    char* out = os_run_capture_raw(argv[1], (void*)0, (void*)0);
    printf("child status: %s", out ? out : "(null)\n");

    if (out && strstr(out, "SigBlk:\t0000000000000000")) {
        printf("PASS: spawned child has a clear signal mask\n");
        return 0;
    }
    printf("FAIL: spawned child inherited the parent's blocked mask\n");
    return 1;
}
