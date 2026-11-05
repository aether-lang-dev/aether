/* Regression: a child started by os.spawn_proc inherits its parent's standard
 * streams even when the parent has marked them close-on-exec, as Node does for
 * 0, 1 and 2. Without the fix exec closed them, and the child started with no
 * stdin, stdout or stderr: chromedriver, spawned this way from a Node program,
 * could not start Chrome ("Failed to create Chrome process").
 *
 * Aether cannot set close-on-exec from the language, so this is a C harness:
 * it marks 0, 1 and 2 close-on-exec, then drives the real os_spawn_raw on a
 * probe that reports which of its own streams are open.
 *
 * Then a parent with 0 and 2 closed (#3170). A spawn reads a failed exec's
 * errno from a pipe, which takes the lowest free descriptors: here 0 and 2.
 * The child has to move the write end off 2 and close it there before the
 * exec. Left open, the child keeps it, and the spawn waits for the child to
 * end instead of returning once it has started.
 *
 * Then every set of 0, 1 and 2 closed in the parent (#3199). The pipes and
 * the file a spawn makes take those slots, and the child installs its own
 * stdio from them: installed in place, one overwrote another still to be
 * installed, or the close of an original closed what had been installed.
 * os.run_full's child lost its stdout with the parent's 1 or 2 closed, and
 * os.spawn_proc_to's child its output file with the parent's 0 closed.
 *
 * Then os.run_pipe (#3200). Its child used to call setenv between fork and
 * exec, which a child of a threaded parent must not: it mallocs and takes
 * the environment's lock, and a lock another thread held at the fork stays
 * held in the child. That deadlock depends on timing, so the harness pins
 * the cause instead: it defines setenv itself, the runtime's calls bind to
 * it, and a child that calls it exits 97. The child must still see
 * AETHER_IPC_FD=3, once, although this process has one of its own. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct { int _0; const char* _1; } tuple_int_string;
extern tuple_int_string os_spawn_raw(const char* prog, void* argv_list, void* env_list);
extern tuple_int_string os_wait_raw(int token);
extern int os_kill_raw(int pid, int sig);
extern void* list_new(void);
extern int list_add_raw(void* list, void* item);
extern void list_free(void* list);
extern void string_release(const char* s);
typedef struct { const void* _0; const void* _1; int _2; const char* _3; } tuple_run_full;
extern tuple_run_full os_run_full_raw(const char* prog, void* argv_list, void* env_list,
                                      const char* stdin_data, int stdin_len);
extern tuple_int_string os_spawn_to_raw(const char* prog, void* argv_list, void* env_list,
                                        const char* out_path);
extern const char* aether_string_data(const void* s);

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

typedef struct { int _0; int _1; const char* _2; } tuple_int_int_string;
extern tuple_int_int_string os_run_pipe_raw(const char* prog, void* argv_list, void* env_list);
extern tuple_int_string os_wait_pid_raw(int pid);

/* run_pipe's child sees AETHER_IPC_FD=3 exactly once and writes to fd 3,
 * without a setenv between fork and exec. */
static int run_pipe_env(void) {
    static char own[] = "AETHER_IPC_FD=9";
    putenv(own);
    void* av = list_new();
    list_add_raw(av, (void*)"-c");
    list_add_raw(av, (void*)"n=$(env | grep -c '^AETHER_IPC_FD='); "
                            "[ \"$AETHER_IPC_FD\" = 3 ] && [ \"$n\" = 1 ] && echo ok >&3");
    tuple_int_int_string sp = os_run_pipe_raw("/bin/sh", av, (void*)0);
    list_free(av);
    if (sp._2 && sp._2[0]) { printf("FAIL: run_pipe: %s\n", sp._2); return 1; }
    char got[16] = { 0 };
    ssize_t k = read(sp._0, got, sizeof got - 1);
    if (k < 0) got[0] = '\0';
    close(sp._0);
    tuple_int_string w = os_wait_pid_raw(sp._1);
    if (w._0 == 97) {
        printf("FAIL: run_pipe's child called setenv between fork and exec\n");
        return 1;
    }
    if (w._0 != 0 || strcmp(got, "ok\n") != 0) {
        printf("FAIL: run_pipe's child exited %d and wrote '%s': AETHER_IPC_FD was not 3, once\n", w._0, got);
        return 1;
    }
    printf("PASS: run_pipe's child sees AETHER_IPC_FD=3 once, with no setenv between fork and exec\n");
    return 0;
}

static double seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* A child that sleeps 5 s (its stdout off the caller's pipe, so the kill
 * ends all of it), spawned with 0 and 2 closed: the spawn returns at once. */
static int closed_stdio_spawn(void) {
    void* av = list_new();
    list_add_raw(av, (void*)"-c");
    list_add_raw(av, (void*)"exec sleep 5 >/dev/null");
    int saved0 = fcntl(0, F_DUPFD_CLOEXEC, 10);
    int saved2 = fcntl(2, F_DUPFD_CLOEXEC, 10);
    close(0);
    close(2);
    double start = seconds();
    tuple_int_string sp = os_spawn_raw("/bin/sh", av, (void*)0);
    list_free(av);
    double took = seconds() - start;
    dup2(saved0, 0);
    dup2(saved2, 2);
    close(saved0);
    close(saved2);
    if (sp._0 < 0) { printf("FAIL: spawn with 0 and 2 closed: %s\n", sp._1 ? sp._1 : "?"); return 1; }
    os_kill_raw(sp._0, 9);
    (void)os_wait_raw(sp._0);
    if (took > 2.0) {
        printf("FAIL: a spawn with 0 and 2 closed took %.1f s: the child kept the exec-error pipe\n", took);
        return 1;
    }
    printf("PASS: a spawn with 0 and 2 closed returns once the child starts (%.2f s)\n", took);
    return 0;
}

static const char* text(const void* s) {
    const char* t = s ? aether_string_data(s) : NULL;
    return t ? t : "";
}

/* Each set of the parent's 0, 1 and 2 closed: os.run_full feeds the child
 * stdin and reads its stdout and stderr apart, os.spawn_proc_to writes both
 * streams to a file. */
static int closed_slots(const char* dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/spawn_to.out", dir);
    void* full_av = list_new();
    list_add_raw(full_av, (void*)"-c");
    list_add_raw(full_av, (void*)"cat; echo out; echo err >&2");
    void* to_av = list_new();
    list_add_raw(to_av, (void*)"-c");
    list_add_raw(to_av, (void*)"echo out; echo err >&2");
    int fails = 0;
    for (int set = 1; set < 8; set++) {
        int saved[3];
        for (int fd = 0; fd < 3; fd++) saved[fd] = fcntl(fd, F_DUPFD_CLOEXEC, 10);
        for (int fd = 0; fd < 3; fd++) {
            if (set & (1 << fd)) close(fd);
        }
        tuple_run_full r = os_run_full_raw("/bin/sh", full_av, (void*)0, "in\n", 3);
        tuple_int_string t = os_spawn_to_raw("/bin/sh", to_av, (void*)0, path);
        tuple_int_string w = { -1, "" };
        if (t._0 > 0) w = os_wait_raw(t._0);
        for (int fd = 0; fd < 3; fd++) {
            dup2(saved[fd], fd);
            close(saved[fd]);
        }
        char closed[8];
        int n = 0;
        for (int fd = 0; fd < 3; fd++) {
            if (set & (1 << fd)) closed[n++] = (char)('0' + fd);
        }
        closed[n] = '\0';
        if (strcmp(text(r._0), "in\nout\n") != 0 || strcmp(text(r._1), "err\n") != 0 || r._2 != 0) {
            printf("FAIL: run_full with %s closed in the parent: stdout '%s' stderr '%s' exit %d err '%s'\n",
                   closed, text(r._0), text(r._1), r._2, r._3 ? r._3 : "");
            fails++;
        }
        char got[64] = { 0 };
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            ssize_t k = read(fd, got, sizeof got - 1);
            if (k < 0) got[0] = '\0';
            close(fd);
        }
        if (t._0 <= 0 || w._0 != 0 || strcmp(got, "out\nerr\n") != 0) {
            printf("FAIL: spawn_proc_to with %s closed in the parent: token %d exit %d file '%s' err '%s'\n",
                   closed, t._0, w._0, got, t._1 ? t._1 : "");
            fails++;
        }
        /* The captures run_full hands back are the caller's. */
        string_release((const char*)r._0);
        string_release((const char*)r._1);
    }
    list_free(full_av);
    list_free(to_av);
    if (fails) return 1;
    printf("PASS: run_full and spawn_proc_to wire their stdio with any of 0, 1 and 2 closed in the parent\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <probe-script> <scratch-dir>\n", argv[0]); return 2; }
    harness_pid = getpid();

    for (int fd = 0; fd <= 2; fd++) {
        int fl = fcntl(fd, F_GETFD);
        if (fl < 0) { printf("FAIL: fd %d is not open in the parent (test setup)\n", fd); return 1; }
        fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
        /* Sanity: the parent really has the flag, so a pass means the child
         * diverged from it. */
        if (!(fcntl(fd, F_GETFD) & FD_CLOEXEC)) { printf("FAIL: could not mark fd %d (test setup)\n", fd); return 1; }
    }

    tuple_int_string sp = os_spawn_raw(argv[1], (void*)0, (void*)0);
    if (sp._0 < 0) { printf("FAIL: spawn: %s\n", sp._1 ? sp._1 : "?"); return 1; }
    tuple_int_string w = os_wait_raw(sp._0);
    if (w._0 != 0) { printf("FAIL: the probe exited %d: a standard stream was closed in the child\n", w._0); return 1; }
    printf("PASS: the spawned child has stdin, stdout and stderr\n");
    int rc = closed_stdio_spawn();
    rc |= closed_slots(argv[2]);
    return run_pipe_env() | rc;
}
