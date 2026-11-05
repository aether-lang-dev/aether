/* Regression (#3200): a child spawn_sandboxed starts calls nothing between
 * fork and exec that is not async-signal-safe.
 *
 * The child used to setenv LD_PRELOAD and the grant source, execlp the
 * program and print with perror or fprintf there. A child of a threaded
 * parent may do none of that: another thread may have held malloc's lock or
 * the environment's at the fork, and the child never gets it back, so it
 * hangs before it runs the program. That depends on timing, so this harness
 * pins the cause instead: it defines setenv itself, the runtime's calls bind
 * to it, and a child that calls it exits 97. The parent now builds the env,
 * resolves the program on PATH and makes both argvs, and the child calls
 * execve.
 *
 * The cases, each a program spawn_sandboxed starts under the catch-all
 * grant:
 *   - `true`, a bare name: found on PATH, exits 0 (97 before the fix);
 *   - a script that checks the env it got: AETHER_SANDBOX_SHM set,
 *     AETHER_SANDBOX_VERBOSE=0, LD_PRELOAD the sandbox library (and on
 *     FreeBSD AETHER_CAPSICUM=1);
 *   - a script with no #! line, which runs under /bin/sh as execlp ran it,
 *     and exits with its argument;
 *   - a program that is not there: -1, an error, as std.os reports a
 *     program that cannot start (#3170), never an exit of 127. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int aether_spawn_sandboxed(void* grant_list, const char* program, const char* arg);
extern void* list_new(void);
extern int list_add_raw(void* list, void* item);
extern void list_free(void* list);

static pid_t harness_pid;

/* Every setenv the runtime makes binds here. In a child, between fork and
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
    if (argc < 2) {
        fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 2;
    }
    harness_pid = getpid();
    void* all = list_new();
    list_add_raw(all, (void*)"*");
    list_add_raw(all, (void*)"*");
    int fails = 0;

    fails += expect("a bare name runs", aether_spawn_sandboxed(all, "true", NULL), 0);

    char env_script[4096];
    snprintf(env_script, sizeof env_script, "%s/env.sh", argv[1]);
    if (write_script(env_script,
            "#!/bin/sh\n"
            "[ -n \"$AETHER_SANDBOX_SHM\" ] || exit 3\n"
            "[ \"$AETHER_SANDBOX_VERBOSE\" = 0 ] || exit 4\n"
#if defined(__FreeBSD__)
            "[ \"$AETHER_CAPSICUM\" = 1 ] || exit 6\n"
#endif
            "case \"$LD_PRELOAD\" in *libaether_sandbox.so) exit 0 ;; esac\n"
            "exit 5\n") != 0) {
        printf("FAIL cannot write %s\n", env_script);
        return 1;
    }
    fails += expect("the child's env has the sandbox's variables",
                    aether_spawn_sandboxed(all, env_script, NULL), 0);

    char plain_script[4096];
    snprintf(plain_script, sizeof plain_script, "%s/no_shebang", argv[1]);
    if (write_script(plain_script, "exit $1\n") != 0) {
        printf("FAIL cannot write %s\n", plain_script);
        return 1;
    }
    fails += expect("a script with no #! line runs under /bin/sh",
                    aether_spawn_sandboxed(all, plain_script, "6"), 6);

    fails += expect("a program that is not there",
                    aether_spawn_sandboxed(all, "aether_no_such_program_3200", NULL), -1);

    list_free(all);
    if (fails) return 1;
    printf("PASS: spawn_sandboxed's children call only async-signal-safe functions\n");
    return 0;
}
