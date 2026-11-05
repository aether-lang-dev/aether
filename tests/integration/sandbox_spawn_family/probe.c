/* One way of starting a program, run under libaether_sandbox.so (see
 * test_sandbox_spawn_family.sh). Prints "<mode>: RAN" when the program it
 * starts really ran, "<mode>: refused" when it did not, "<mode>: n/a" when
 * this libc has no such call. Each mode starts $PROBE_TRUE (true found on
 * PATH for the p forms) or, for the shell modes, /bin/sh -c 'exit 0'. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <wordexp.h>
#include <sys/wait.h>
extern char** environ;

static int exited_zero(pid_t p) {
    int st;
    return p > 0 && waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* The exec family replaces the process: fork first (the grid grants fork
 * where it means to test the exec), then the call in the child. */
static const char* TRUE_BIN = "/bin/true";

static int exec_in_child(const char* mode, const char* script) {
    pid_t p = fork();
    if (p < 0) return 0;
    if (p == 0) {
        char* const av[] = { "true", NULL };
        if (!strcmp(mode, "execve")) execve(TRUE_BIN, av, environ);
        else if (!strcmp(mode, "execv")) execv(TRUE_BIN, av);
        else if (!strcmp(mode, "execvp")) execvp("true", av);
#if defined(__GLIBC__)
        else if (!strcmp(mode, "execvpe")) execvpe("true", av, environ);
#endif
        else if (!strcmp(mode, "execl")) execl(TRUE_BIN, "true", (char*)0);
        else if (!strcmp(mode, "execlp")) execlp("true", "true", (char*)0);
        else if (!strcmp(mode, "execle")) execle(TRUE_BIN, "true", (char*)0, environ);
        else if (!strcmp(mode, "fexecve")) {
            int fd = open(TRUE_BIN, O_RDONLY);
            if (fd >= 0) fexecve(fd, av, environ);
        }
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
        else if (!strcmp(mode, "execveat")) {
            /* The name relative to a directory descriptor. */
            char dir[4096];
            strncpy(dir, TRUE_BIN, sizeof(dir) - 1);
            dir[sizeof(dir) - 1] = '\0';
            char* slash = strrchr(dir, '/');
            if (slash) {
                *slash = '\0';
                int dfd = open(dir[0] ? dir : "/", O_RDONLY | O_DIRECTORY);
                if (dfd >= 0) execveat(dfd, slash + 1, av, environ, 0);
            }
        } else if (!strcmp(mode, "execveat_fd")) {
            /* The descriptor itself. */
            int fd = open(TRUE_BIN, O_RDONLY);
            if (fd >= 0) execveat(fd, "", av, environ, AT_EMPTY_PATH);
        }
#endif
        else if (!strcmp(mode, "script_execvp")) {
            /* No #! line: execvp runs it with /bin/sh. */
            char* const sv[] = { (char*)script, NULL };
            execvp(script, sv);
        }
        _exit(42);
    }
    return exited_zero(p);
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const char* m = argv[1];
    const char* script = argc > 2 ? argv[2] : "";
    const char* t = getenv("PROBE_TRUE");
    if (t && *t) TRUE_BIN = t;
    char* const av[] = { "true", NULL };
    int ran = 0;
    if (!strcmp(m, "system")) ran = system("exit 0") == 0;
    else if (!strcmp(m, "strip_system")) {
        /* The shell inherits the live environment: one that has lost the
         * preload must not be handed a shell. */
        unsetenv("LD_PRELOAD");
        ran = system("exit 0") == 0;
    } else if (!strcmp(m, "popen")) {
        FILE* f = popen("exit 0", "r");
        ran = f && pclose(f) == 0;
    } else if (!strcmp(m, "posix_spawn")) {
        pid_t p = -1;
        ran = posix_spawn(&p, TRUE_BIN, NULL, NULL, av, environ) == 0 && exited_zero(p);
    } else if (!strcmp(m, "posix_spawnp")) {
        pid_t p = -1;
        ran = posix_spawnp(&p, "true", NULL, NULL, av, environ) == 0 && exited_zero(p);
    } else if (!strcmp(m, "pidfd_spawn") || !strcmp(m, "pidfd_spawnp")) {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 39))
        int fd = -1;
        int rc = !strcmp(m, "pidfd_spawn")
            ? pidfd_spawn(&fd, TRUE_BIN, NULL, NULL, av, environ)
            : pidfd_spawnp(&fd, "true", NULL, NULL, av, environ);
        if (rc == 0) {
            siginfo_t si;
            ran = waitid(P_PIDFD, (id_t)fd, &si, WEXITED) == 0 && si.si_code == CLD_EXITED && si.si_status == 0;
            close(fd);
        }
#else
        printf("%s: n/a\n", m);
        return 0;
#endif
    } else if (!strcmp(m, "_Fork")) {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
        pid_t p = _Fork();
        if (p == 0) _exit(0);
        ran = exited_zero(p);
#else
        printf("%s: n/a\n", m);
        return 0;
#endif
    } else if (!strcmp(m, "wordexp_cmd")) {
        wordexp_t we;
        int rc = wordexp("$(echo hi)", &we, 0);
        ran = rc == 0 && we.we_wordc == 1 && !strcmp(we.we_wordv[0], "hi");
        if (rc == 0) wordfree(&we);
    } else if (!strcmp(m, "wordexp_plain")) {
        /* A word with no command substitution needs no shell (glibc). */
        wordexp_t we;
        int rc = wordexp("plain words", &we, 0);
        ran = rc == 0 && we.we_wordc == 2;
        if (rc == 0) wordfree(&we);
    } else {
#if !defined(__GLIBC__)
        if (!strcmp(m, "execvpe")) { printf("%s: n/a\n", m); return 0; }
#endif
#if !(defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34)))
        if (!strcmp(m, "execveat") || !strcmp(m, "execveat_fd")) { printf("%s: n/a\n", m); return 0; }
#endif
        ran = exec_in_child(m, script);
    }
    printf("%s: %s\n", m, ran ? "RAN" : "refused");
    return 0;
}
