/* Regression (#3200): contrib's process host (contrib/host/aether)
 * prepares its child in the parent, and the child calls nothing between
 * fork and exec that is not async-signal-safe. (The Go host this also
 * covered moved to aether-guest-languages, go/, and is tested there.)
 *
 * Its children used to setenv LD_PRELOAD, the grant source and the map
 * between fork and exec. A child of a
 * threaded parent may do none of that: another thread may have held
 * malloc's lock or the environment's at the fork, and the child never gets
 * it back. That depends on timing, so this harness pins the cause instead:
 * it defines setenv itself, the host's calls bind to it, and a child that
 * calls it exits 97.
 *
 * The host is compiled into this harness by the runner, under the
 * catch-all grant:
 *   - the Aether host captures a script that prints the sandbox's variables
 *     from its env: AETHER_SANDBOX_SHM set and LD_PRELOAD the library;
 *   - the Aether host runs `true` by its path: 0;
 *   - the Aether host runs a binary that is not there: -1, an error, as
 *     std.os reports a program that cannot start (#3170), never an exit of
 *     127. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int aether_host_run_sandboxed(void* perms, const char* binary_path);
extern char* aether_host_capture_sandboxed(void* perms, const char* binary_path);
extern void* list_new(void);
extern int list_add_raw(void* list, void* item);
extern void list_free(void* list);

static pid_t harness_pid;

/* Every setenv the host makes binds here. In a child, between fork and
 * exec, it ends the child with 97; in this process nothing calls it. */
int setenv(const char* name, const char* value, int overwrite) {
    (void)name;
    (void)value;
    (void)overwrite;
    if (getpid() != harness_pid) _exit(97);
    errno = ENOSYS;
    return -1;
}

static int write_script(const char* path, const char* text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) return -1;
    size_t len = strlen(text);
    ssize_t w = write(fd, text, len);
    close(fd);
    if (w != (ssize_t)len) return -1;
    return chmod(path, 0755);
}

static int expect(const char* what, int rc, int want) {
    if (rc == want) {
        printf("ok   %s (%d)\n", what, rc);
        return 0;
    }
    if (rc == 97) printf("FAIL %s: the child called setenv between fork and exec\n", what);
    else printf("FAIL %s: %d, expected %d\n", what, rc, want);
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <scratch-dir> <path-of-true>\n", argv[0]);
        return 2;
    }
    harness_pid = getpid();
    void* all = list_new();
    list_add_raw(all, (void*)"*");
    list_add_raw(all, (void*)"*");
    int fails = 0;

    char env_script[4096];
    snprintf(env_script, sizeof env_script, "%s/env.sh", argv[1]);
    if (write_script(env_script,
            "#!/bin/sh\n"
            "[ -n \"$AETHER_SANDBOX_SHM\" ] || { echo no-shm; exit 0; }\n"
            "case \"$LD_PRELOAD\" in *libaether_sandbox.so) echo env-ok ;; *) echo no-preload ;; esac\n") != 0) {
        printf("FAIL cannot write %s\n", env_script);
        return 1;
    }
    char* got = aether_host_capture_sandboxed(all, env_script);
    if (got && strcmp(got, "env-ok\n") == 0) {
        printf("ok   the Aether host's child has the sandbox's variables\n");
    } else {
        printf("FAIL the Aether host's child printed '%s', expected env-ok "
               "(nothing at all when it called setenv)\n", got ? got : "(null)");
        fails++;
    }
    free(got);

    fails += expect("the Aether host runs a program by its path",
                    aether_host_run_sandboxed(all, argv[2]), 0);
    fails += expect("the Aether host's binary that is not there",
                    aether_host_run_sandboxed(all, "/no/such/aether_binary_3170"), -1);

    list_free(all);
    if (fails) return 1;
    printf("PASS: the process host's children call only async-signal-safe functions\n");
    return 0;
}
