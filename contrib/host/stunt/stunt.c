/* stunt.c: the engine (see stunt.h and README.md).
 *
 * Every check does one thing a hosted language can do, the way a real
 * interpreter's runtime would do it: through libc (Lua's io.open, Python's
 * os.getenv), past libc (ctypes reaching libc's syscall(), a raw syscall
 * instruction), or through the host (a registered function, the shared
 * map). The engine judges the outcome itself where it can: a refusal is
 * the error a sandbox gives (EACCES or EPERM from libc, the preload or the
 * kernel), or a host function's error that says so by starting "refused:";
 * anything else that fails is reported as an error, never as a refusal, so
 * a missing file, an unset variable or an unreachable network cannot pass
 * for a sandbox at work.
 *
 * Built two ways from this one file: a shared library a bridge dlopens
 * (libstunt), and, with stunt_main.c, the stunt program a host spawns.
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* dladdr, RTLD_* on glibc */
#endif

#include "stunt.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__) || defined(__FreeBSD__)
#include <sys/syscall.h>
#endif
#endif

#if defined(__STDC_NO_ATOMICS__)
typedef volatile int st_flag;
#define ST_FLAG_SET(f, v) ((f) = (v))
#define ST_FLAG_GET(f) (f)
#else
#include <stdatomic.h>
typedef atomic_int st_flag;
#define ST_FLAG_SET(f, v) atomic_store(&(f), (v))
#define ST_FLAG_GET(f) atomic_load(&(f))
#endif

#if defined(_WIN32)
#define ST_ENVIRON _environ
#else
extern char** environ;
#define ST_ENVIRON environ
#endif

/* ---- the VM -------------------------------------------------------------- */

#define ST_MAX_FNS  32
#define ST_MAX_ARGS 4
#define ST_LINE     1024

typedef struct {
    int type;
    int64_t i;
    const char* s;    /* an argument: borrowed for the call */
} st_val;

struct st_vm {
    struct { char* name; st_host_fn fn; } fns[ST_MAX_FNS];
    int nfns;
    char** env;        /* the copy of the environment taken by st_new */
    int nenv;
    char** hidden;     /* names kept from the copy: scrubbed, or withheld by the host */
    int nhidden;
    int64_t budget;
    st_flag cancelled;
    st_interrupt_fn interrupt;
    void* interrupt_ud;
    /* the host call in progress */
    const char* call_name;
    st_val args[ST_MAX_ARGS];
    int nargs;
    int ret_type;
    int64_t ret_i;
    char* ret_s;       /* owned */
    char* error;       /* owned; set by st_raise_error */
    char result[ST_LINE];
};

#define ST_WITHHELD "STUNT_WITHHELD"

/* Records `name` as kept from the engine: by st_env_scrub, or by the host. */
static void hide(st_vm* vm, const char* name) {
    for (int i = 0; i < vm->nhidden; i++) {
        if (strcmp(vm->hidden[i], name) == 0) return;
    }
    char** grown = (char**)realloc(vm->hidden, ((size_t)vm->nhidden + 1) * sizeof(char*));
    if (!grown) return;
    vm->hidden = grown;
    char* copy = strdup(name);
    if (copy) vm->hidden[vm->nhidden++] = copy;
}

static int hidden(st_vm* vm, const char* name) {
    for (int i = 0; i < vm->nhidden; i++) {
        if (strcmp(vm->hidden[i], name) == 0) return 1;
    }
    return 0;
}

st_vm* st_new(void) {
    st_vm* vm = (st_vm*)calloc(1, sizeof *vm);
    if (!vm) return NULL;
    ST_FLAG_SET(vm->cancelled, 0);
    char** env = ST_ENVIRON;
    int n = 0;
    while (env && env[n]) n++;
    vm->env = (char**)calloc((size_t)n + 1, sizeof(char*));
    if (vm->env) {
        for (int i = 0; i < n; i++) {
            char* copy = strdup(env[i]);
            if (copy) vm->env[vm->nenv++] = copy;
        }
    }
    /* A host that starts the engine in a child with less than its own
     * environment names what it kept back in STUNT_WITHHELD, a comma list;
     * that is how a missing variable is told from one the host never had. */
    for (int i = 0; i < n; i++) {
        if (strncmp(env[i], ST_WITHHELD "=", sizeof ST_WITHHELD) != 0) continue;
        const char* p = env[i] + sizeof ST_WITHHELD;
        while (*p) {
            size_t len = strcspn(p, ",");
            if (len > 0 && len < 256) {
                char name[256];
                memcpy(name, p, len);
                name[len] = '\0';
                hide(vm, name);
            }
            p += len;
            if (*p == ',') p++;
        }
    }
    return vm;
}

static void clear_call(st_vm* vm) {
    free(vm->ret_s);
    vm->ret_s = NULL;
    free(vm->error);
    vm->error = NULL;
    vm->ret_type = ST_TNIL;
    vm->ret_i = 0;
    vm->nargs = 0;
    vm->call_name = NULL;
}

void st_free(st_vm* vm) {
    if (!vm) return;
    clear_call(vm);
    for (int i = 0; i < vm->nfns; i++) free(vm->fns[i].name);
    for (int i = 0; i < vm->nenv; i++) free(vm->env[i]);
    free(vm->env);
    for (int i = 0; i < vm->nhidden; i++) free(vm->hidden[i]);
    free(vm->hidden);
    free(vm);
}

int st_register(st_vm* vm, const char* name, st_host_fn fn) {
    if (!vm || !name || !fn) return -1;
    for (int i = 0; i < vm->nfns; i++) {
        if (strcmp(vm->fns[i].name, name) == 0) { vm->fns[i].fn = fn; return 0; }
    }
    if (vm->nfns >= ST_MAX_FNS) return -1;
    char* copy = strdup(name);
    if (!copy) return -1;
    vm->fns[vm->nfns].name = copy;
    vm->fns[vm->nfns].fn = fn;
    vm->nfns++;
    return 0;
}

void st_set_instruction_limit(st_vm* vm, int64_t budget) { if (vm) vm->budget = budget; }
void st_cancel(st_vm* vm) { if (vm) ST_FLAG_SET(vm->cancelled, 1); }
void st_set_interrupt_handler(st_vm* vm, st_interrupt_fn fn, void* ud) {
    if (!vm) return;
    vm->interrupt = fn;
    vm->interrupt_ud = ud;
}

void st_env_scrub(st_vm* vm, int (*keep)(const char* name)) {
    if (!vm || !keep) return;
    int w = 0;
    for (int r = 0; r < vm->nenv; r++) {
        char* e = vm->env[r];
        char name[256];
        const char* eq = strchr(e, '=');
        size_t n = eq ? (size_t)(eq - e) : strlen(e);
        if (n >= sizeof name) n = sizeof name - 1;
        memcpy(name, e, n);
        name[n] = '\0';
        if (keep(name)) vm->env[w++] = e;
        else { hide(vm, name); free(e); }
    }
    vm->nenv = w;
    if (vm->env) vm->env[w] = NULL;
}

static const char* cached_env(st_vm* vm, const char* name) {
    size_t n = strlen(name);
    for (int i = 0; i < vm->nenv; i++) {
        if (strncmp(vm->env[i], name, n) == 0 && vm->env[i][n] == '=') return vm->env[i] + n + 1;
    }
    return NULL;
}

/* ---- host calls ---------------------------------------------------------- */

const char* st_call_name(void* v) { st_vm* vm = (st_vm*)v; return vm && vm->call_name ? vm->call_name : ""; }
int st_arg_count(void* v) { st_vm* vm = (st_vm*)v; return vm ? vm->nargs : 0; }
int st_arg_type(void* v, int i) {
    st_vm* vm = (st_vm*)v;
    return (vm && i >= 1 && i <= vm->nargs) ? vm->args[i - 1].type : ST_TNIL;
}
const char* st_arg_str(void* v, int i) {
    st_vm* vm = (st_vm*)v;
    if (!vm || i < 1 || i > vm->nargs) return "";
    return vm->args[i - 1].type == ST_TSTR && vm->args[i - 1].s ? vm->args[i - 1].s : "";
}
int64_t st_arg_int(void* v, int i) {
    st_vm* vm = (st_vm*)v;
    if (!vm || i < 1 || i > vm->nargs) return 0;
    return vm->args[i - 1].type == ST_TINT ? vm->args[i - 1].i : 0;
}
void st_push_str(void* v, const char* s) {
    st_vm* vm = (st_vm*)v;
    if (!vm) return;
    free(vm->ret_s);
    vm->ret_s = strdup(s ? s : "");
    vm->ret_type = vm->ret_s ? ST_TSTR : ST_TNIL;
}
void st_push_int(void* v, int64_t i) {
    st_vm* vm = (st_vm*)v;
    if (!vm) return;
    free(vm->ret_s);
    vm->ret_s = NULL;
    vm->ret_type = ST_TINT;
    vm->ret_i = i;
}
void st_push_nil(void* v) {
    st_vm* vm = (st_vm*)v;
    if (!vm) return;
    free(vm->ret_s);
    vm->ret_s = NULL;
    vm->ret_type = ST_TNIL;
}
int st_raise_error(void* v, const char* msg) {
    st_vm* vm = (st_vm*)v;
    if (!vm) return -1;
    free(vm->error);
    vm->error = strdup(msg && *msg ? msg : "error");
    return -1;
}

/* Calls host function `name` with `nargs` arguments. 0 with the result in
 * vm->ret_*, or -1 with a reason in `why`. */
static int host_call(st_vm* vm, const char* name, int nargs, const st_val* args, char* why, size_t cap) {
    st_host_fn fn = NULL;
    st_host_fn fallback = NULL;
    for (int i = 0; i < vm->nfns; i++) {
        if (strcmp(vm->fns[i].name, name) == 0) fn = vm->fns[i].fn;
        else if (strcmp(vm->fns[i].name, "*") == 0) fallback = vm->fns[i].fn;
    }
    if (!fn) fn = fallback;
    clear_call(vm);
    if (!fn) {
        snprintf(why, cap, "no host function %s", name);
        return -1;
    }
    vm->call_name = name;
    vm->nargs = nargs > ST_MAX_ARGS ? ST_MAX_ARGS : nargs;
    for (int i = 0; i < vm->nargs; i++) vm->args[i] = args[i];
    int pushed = fn(vm);
    vm->call_name = NULL;
    vm->nargs = 0;
    if (vm->error || pushed < 0) {
        snprintf(why, cap, "%s", vm->error ? vm->error : "host function failed");
        return -1;
    }
    return 0;
}

static st_val sval(const char* s) { st_val v; v.type = ST_TSTR; v.i = 0; v.s = s; return v; }
static st_val ival(int64_t i) { st_val v; v.type = ST_TINT; v.i = i; v.s = NULL; return v; }

/* ---- outcomes ------------------------------------------------------------ */

enum { OUT_ERROR = -1, OUT_REFUSED = 0, OUT_SUCCEEDED = 1, OUT_STOPPED = 2 };

static int detail(char* d, size_t cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d, cap, fmt, ap);
    va_end(ap);
    return 0;
}

#if !defined(_WIN32)
/* What a failed libc call's errno says: a sandbox refusal or an error. */
static int by_errno(int e, char* d, size_t cap, const char* what) {
    detail(d, cap, "%s%s%s", what, *what ? ": " : "", strerror(e));
    return (e == EACCES || e == EPERM) ? OUT_REFUSED : OUT_ERROR;
}
#endif

#define FIXTURE "stunt-fixture.txt"
#define WRITTEN "stunt-written.txt"

/* A host function's error is a refusal only when the host says it is one:
 * its reason starts "refused:" (the bridge's and host.ae's read_file and
 * getenv do this when the grants say no). A missing file, a failed read or
 * no such function is an error, never a refusal. */
static int host_refused(const char* why) { return strncmp(why, "refused:", 8) == 0; }
static int host_failed(const char* why, char* d, size_t cap) {
    detail(d, cap, "%s", why);
    return host_refused(why) ? OUT_REFUSED : OUT_ERROR;
}

static void first_line(const char* s, char* out, size_t cap) {
    size_t n = strcspn(s, "\r\n");
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

/* ---- the checks: through libc -------------------------------------------- */

#if defined(_WIN32)
#define NOT_HERE(d, cap) (detail(d, cap, "not on Windows"), OUT_ERROR)
#endif

static int c_open_path(const char* path, char* d, size_t cap) {
#if defined(_WIN32)
    (void)path;
    return NOT_HERE(d, cap);
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) return by_errno(errno, d, cap, "");
    char buf[128];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    buf[n > 0 ? n : 0] = '\0';
    char line[96];
    first_line(buf, line, sizeof line);
    detail(d, cap, "read \"%s\"", line);
    return OUT_SUCCEEDED;
#endif
}

static int c_open_etc_passwd(st_vm* vm, char* d, size_t cap) { (void)vm; return c_open_path("/etc/passwd", d, cap); }
static int c_open_granted_file(st_vm* vm, char* d, size_t cap) { (void)vm; return c_open_path(FIXTURE, d, cap); }

static int c_write_granted_file(st_vm* vm, char* d, size_t cap) {
    (void)vm;
#if defined(_WIN32)
    return NOT_HERE(d, cap);
#else
    int fd = open(WRITTEN, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return by_errno(errno, d, cap, "");
    const char* text = "written by stunt\n";
    ssize_t n = write(fd, text, strlen(text));
    int e = errno;
    close(fd);
    if (n < 0) return by_errno(e, d, cap, "write");
    detail(d, cap, "wrote %s", WRITTEN);
    return OUT_SUCCEEDED;
#endif
}

/* Whether `name` is in the process's environment, read past getenv (and
 * so past any preload's getenv): how a getenv that says NULL for a variable
 * that is there is told from one that is simply unset. */
static int in_environ(const char* name) {
    size_t n = strlen(name);
    for (char** e = ST_ENVIRON; e && *e; e++) {
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') return 1;
    }
    return 0;
}

/* A NULL getenv is a refusal only with evidence of one: the variable is in
 * the environment and getenv would not give it (the preload), or the host
 * withheld it from this process. HOME simply unset is an error. */
static int c_getenv_home(st_vm* vm, char* d, size_t cap) {
    const char* v = getenv("HOME");
    if (v) { detail(d, cap, "HOME=%s", v); return OUT_SUCCEEDED; }
    if (in_environ("HOME")) { detail(d, cap, "getenv gave NULL; HOME is in the environment"); return OUT_REFUSED; }
    if (hidden(vm, "HOME")) { detail(d, cap, "the host withheld HOME"); return OUT_REFUSED; }
    detail(d, cap, "HOME is unset");
    return OUT_ERROR;
}

/* HOME missing from the copy is a refusal when the scrub took it out or the
 * host withheld it; when it was never there, an error. */
static int c_env_cached(st_vm* vm, char* d, size_t cap) {
    const char* v = cached_env(vm, "HOME");
    if (v) { detail(d, cap, "HOME=%s from the copy", v); return OUT_SUCCEEDED; }
    if (hidden(vm, "HOME")) { detail(d, cap, "HOME was kept from the copy"); return OUT_REFUSED; }
    detail(d, cap, "HOME was never in the environment");
    return OUT_ERROR;
}

#if !defined(_WIN32)
/* A non-blocking connect: the sandbox refuses at once, with EACCES from the
 * preload or Landlock at connect(), or EPERM from seccomp at socket(). EPERM
 * at connect() is not the sandbox's: it is what a netfilter OUTPUT drop
 * gives, so on a firewalled box it would let a sandbox that failed open read
 * as refused; it is an error. Anything else, EINPROGRESS or a network error,
 * means the call was let through.
 *
 * The address is TEST-NET-1's 192.0.2.1 (RFC 5737), reserved for
 * documentation: nothing real answers there, so a check that is let through
 * contacts nobody. Non-blocking, the connect cannot hang on it either; a
 * connect still in progress is waited on for CONNECT_WAIT_MS at most, for an
 * answer that comes back at once, and then dropped. */
#define CONNECT_ADDR "192.0.2.1"
#define CONNECT_WAIT_MS 100
static int connect_to(int family, const char* addr, char* d, size_t cap) {
    int s = socket(family, SOCK_STREAM, 0);
    if (s < 0) return by_errno(errno, d, cap, "socket");
    int fl = fcntl(s, F_GETFL, 0);
    if (fl >= 0) (void)fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int r;
    if (family == AF_INET6) {
        struct sockaddr_in6 sa;
        memset(&sa, 0, sizeof sa);
        sa.sin6_family = AF_INET6;
        sa.sin6_port = htons(443);
        if (inet_pton(AF_INET6, addr, &sa.sin6_addr) != 1) { close(s); detail(d, cap, "bad address"); return OUT_ERROR; }
        r = connect(s, (struct sockaddr*)&sa, sizeof sa);
    } else {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons(443);
        if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) { close(s); detail(d, cap, "bad address"); return OUT_ERROR; }
        r = connect(s, (struct sockaddr*)&sa, sizeof sa);
    }
    int e = errno;
    if (r != 0 && e == EINPROGRESS) {
        struct pollfd p;
        p.fd = s;
        p.events = POLLOUT;
        p.revents = 0;
        if (poll(&p, 1, CONNECT_WAIT_MS) > 0) {
            int soe = 0;
            socklen_t len = sizeof soe;
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, &soe, &len) == 0) {
                if (soe == 0) r = 0;
                else e = soe;
            }
        }
    }
    close(s);
    if (r == 0) { detail(d, cap, "connected"); return OUT_SUCCEEDED; }
    if (e == EACCES) return by_errno(e, d, cap, "connect");
    if (e == EPERM) { detail(d, cap, "connect: %s (a firewall's answer, not the sandbox's)", strerror(e)); return OUT_ERROR; }
    detail(d, cap, "let through: %s", strerror(e));
    return OUT_SUCCEEDED;
}
#endif

static int c_connect_v4(st_vm* vm, char* d, size_t cap) {
    (void)vm;
#if defined(_WIN32)
    return NOT_HERE(d, cap);
#else
    return connect_to(AF_INET, CONNECT_ADDR, d, cap);
#endif
}

static int c_connect_mapped(st_vm* vm, char* d, size_t cap) {
    (void)vm;
#if defined(_WIN32)
    return NOT_HERE(d, cap);
#else
    return connect_to(AF_INET6, "::ffff:" CONNECT_ADDR, d, cap);
#endif
}

#if !defined(_WIN32)
/* fork, run `child` in the new process, and read its exit code:
 * 0 succeeded, 126 refused, anything else an error. */
static int in_child(void (*child)(void), char* d, size_t cap, const char* what) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) return by_errno(errno, d, cap, "fork");
    if (pid == 0) {
        child();
        _exit(0);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return by_errno(errno, d, cap, "waitpid");
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) { detail(d, cap, "%s ran", what); return OUT_SUCCEEDED; }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 126) { detail(d, cap, "%s: refused", what); return OUT_REFUSED; }
    if (WIFSIGNALED(status)) { detail(d, cap, "%s: killed by signal %d", what, WTERMSIG(status)); return OUT_ERROR; }
    detail(d, cap, "%s: exit %d", what, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return OUT_ERROR;
}

static void exec_sh(void) {
    char* argv[] = { (char*)"sh", (char*)"-c", (char*)"exit 0", NULL };
    execve("/bin/sh", argv, environ);
    _exit((errno == EACCES || errno == EPERM) ? 126 : 127);
}

static void exit_at_once(void) { _exit(0); }
#endif

static int c_exec_bin_sh(st_vm* vm, char* d, size_t cap) {
    (void)vm;
#if defined(_WIN32)
    return NOT_HERE(d, cap);
#else
    return in_child(exec_sh, d, cap, "execve /bin/sh");
#endif
}

static int c_fork(st_vm* vm, char* d, size_t cap) {
    (void)vm;
#if defined(_WIN32)
    return NOT_HERE(d, cap);
#else
    return in_child(exit_at_once, d, cap, "the forked child");
#endif
}

#if !defined(_WIN32)
static int try_dlopen(const char* name, char* d, size_t cap) {
    errno = 0;
    void* h = dlopen(name, RTLD_NOW);
    int e = errno;
    if (h) {
        dlclose(h);
        detail(d, cap, "loaded %s", name);
        return OUT_SUCCEEDED;
    }
    if (e == EACCES) { detail(d, cap, "%s", name); return OUT_REFUSED; }
    const char* why = dlerror();
    detail(d, cap, "%s: %s", name, why ? why : "dlopen failed");
    return OUT_ERROR;
}
#endif

static int c_dlopen_libc_by_name(st_vm* vm, char* d, size_t cap) {
    (void)vm;
#if defined(_WIN32)
    return NOT_HERE(d, cap);
#elif defined(__FreeBSD__)
    return try_dlopen("libc.so.7", d, cap);
#elif defined(__APPLE__)
    return try_dlopen("libSystem.B.dylib", d, cap);
#else
    return try_dlopen("libc.so.6", d, cap);
#endif
}

static int c_dlopen_libc_by_path(st_vm* vm, char* d, size_t cap) {
    (void)vm;
#if defined(_WIN32)
    return NOT_HERE(d, cap);
#else
    Dl_info info;
    if (!dladdr((void*)strlen, &info) || !info.dli_fname || info.dli_fname[0] != '/') {
        detail(d, cap, "cannot find libc's path");
        return OUT_ERROR;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s", info.dli_fname);
    return try_dlopen(path, d, cap);
#endif
}

static int c_dlopen_libm(st_vm* vm, char* d, size_t cap) {
    (void)vm;
#if defined(_WIN32)
    return NOT_HERE(d, cap);
#elif defined(__FreeBSD__)
    return try_dlopen("libm.so.5", d, cap);
#elif defined(__APPLE__)
    return try_dlopen("/usr/lib/libm.dylib", d, cap);
#else
    return try_dlopen("libm.so.6", d, cap);
#endif
}

/* libc's own syscall(): what ctypes, Perl's syscall and Ruby's Fiddle reach.
 * Only EPERM or EACCES is a refusal. ENOSYS is not: the preload's syscall()
 * gives EPERM while its sandbox is active and ENOSYS while it is not, so
 * ENOSYS means a preload whose grants never applied. */
static int c_syscall_fn_openat(st_vm* vm, char* d, size_t cap) {
    (void)vm;
#if (defined(__linux__) || defined(__FreeBSD__)) && defined(SYS_openat)
    errno = 0;
    long r = syscall(SYS_openat, (long)AT_FDCWD, "/etc/passwd", (long)O_RDONLY, 0L);
    if (r >= 0) { close((int)r); detail(d, cap, "opened /etc/passwd"); return OUT_SUCCEEDED; }
    return by_errno(errno, d, cap, "syscall()");
#else
    detail(d, cap, "no syscall() here");
    return OUT_ERROR;
#endif
}

/* The syscall instruction itself: past libc and so past any preload. Only
 * the kernel (Landlock, seccomp) can refuse these. */
#if defined(__linux__) && defined(__x86_64__)
#define ST_NR_OPENAT 257
#define ST_NR_SOCKET 41
static long raw4(long nr, long a, long b, long c, long e) {
    long ret;
    register long r10 __asm__("r10") = e;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}
#define ST_HAVE_RAW 1
#elif defined(__linux__) && defined(__aarch64__)
#define ST_NR_OPENAT 56
#define ST_NR_SOCKET 198
static long raw4(long nr, long a, long b, long c, long e) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = e;
    __asm__ volatile ("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}
#define ST_HAVE_RAW 1
#endif

static int c_syscall_openat(st_vm* vm, char* d, size_t cap) {
    (void)vm;
#ifdef ST_HAVE_RAW
    long r = raw4(ST_NR_OPENAT, -100 /* AT_FDCWD */, (long)"/etc/passwd", O_RDONLY, 0);
    if (r >= 0) { close((int)r); detail(d, cap, "opened /etc/passwd"); return OUT_SUCCEEDED; }
    return by_errno((int)-r, d, cap, "");
#else
    detail(d, cap, "no raw syscall on this platform (Linux x86_64 and aarch64)");
    return OUT_ERROR;
#endif
}

static int c_syscall_socket(st_vm* vm, char* d, size_t cap) {
    (void)vm;
#ifdef ST_HAVE_RAW
    long r = raw4(ST_NR_SOCKET, 2 /* AF_INET */, 1 /* SOCK_STREAM */, 0, 0);
    if (r >= 0) { close((int)r); detail(d, cap, "made an AF_INET socket"); return OUT_SUCCEEDED; }
    return by_errno((int)-r, d, cap, "");
#else
    detail(d, cap, "no raw syscall on this platform (Linux x86_64 and aarch64)");
    return OUT_ERROR;
#endif
}

/* ---- the checks: through the host ---------------------------------------- */

/* A checked binding: refused when the host says "refused:", an error for
 * any other failure, and an error when it answers nothing at all. */
static int c_host_str(st_vm* vm, const char* fn, const char* arg, char* d, size_t cap) {
    st_val a = sval(arg);
    char why[256];
    if (host_call(vm, fn, 1, &a, why, sizeof why) != 0) return host_failed(why, d, cap);
    if (vm->ret_type != ST_TSTR) { detail(d, cap, "%s(\"%s\") gave nothing", fn, arg); return OUT_ERROR; }
    char line[96];
    first_line(vm->ret_s, line, sizeof line);
    detail(d, cap, "%s(\"%s\") = \"%s\"", fn, arg, line);
    return OUT_SUCCEEDED;
}

static int c_binding_read_etc_passwd(st_vm* vm, char* d, size_t cap) { return c_host_str(vm, "read_file", "/etc/passwd", d, cap); }
static int c_binding_read_granted(st_vm* vm, char* d, size_t cap) { return c_host_str(vm, "read_file", FIXTURE, d, cap); }
static int c_binding_getenv_home(st_vm* vm, char* d, size_t cap) { return c_host_str(vm, "getenv", "HOME", d, cap); }

static int c_callback_greet(st_vm* vm, char* d, size_t cap) {
    st_val a = sval("stunt");
    char why[256];
    if (host_call(vm, "greet", 1, &a, why, sizeof why) != 0) return host_failed(why, d, cap);
    if (vm->ret_type != ST_TSTR) { detail(d, cap, "greet returned type %d, not a string", vm->ret_type); return OUT_ERROR; }
    if (strcmp(vm->ret_s, "hello, stunt") != 0) { detail(d, cap, "greet(\"stunt\") = \"%s\"", vm->ret_s); return OUT_ERROR; }
    detail(d, cap, "greet(\"stunt\") = \"hello, stunt\"");
    return OUT_SUCCEEDED;
}

static int c_callback_add(st_vm* vm, char* d, size_t cap) {
    st_val a[2] = { ival(2), ival(40) };
    char why[256];
    if (host_call(vm, "add", 2, a, why, sizeof why) != 0) return host_failed(why, d, cap);
    if (vm->ret_type != ST_TINT) { detail(d, cap, "add returned type %d, not an int", vm->ret_type); return OUT_ERROR; }
    if (vm->ret_i != 42) { detail(d, cap, "add(2, 40) = %lld", (long long)vm->ret_i); return OUT_ERROR; }
    detail(d, cap, "add(2, 40) = 42");
    return OUT_SUCCEEDED;
}

/* The one host error this check counts as a refusal is the host's answer
 * to a name it does not know, "no host function nope"; any other failure
 * (a hang-up, a reply not understood) is an error. */
static int c_callback_unregistered(st_vm* vm, char* d, size_t cap) {
    char why[256];
    if (host_call(vm, "nope", 0, NULL, why, sizeof why) != 0) {
        detail(d, cap, "%s", why);
        return strcmp(why, "no host function nope") == 0 ? OUT_REFUSED : OUT_ERROR;
    }
    detail(d, cap, "nope() answered");
    return OUT_SUCCEEDED;
}

static int c_map_get(st_vm* vm, char* d, size_t cap) {
    st_val a = sval("input");
    char why[256];
    if (host_call(vm, "aether_map_get", 1, &a, why, sizeof why) != 0) return host_failed(why, d, cap);
    if (vm->ret_type != ST_TSTR) { detail(d, cap, "input is not in the map"); return OUT_ERROR; }
    if (strcmp(vm->ret_s, "stunt-in") != 0) { detail(d, cap, "input = \"%s\"", vm->ret_s); return OUT_ERROR; }
    detail(d, cap, "input = \"stunt-in\"");
    return OUT_SUCCEEDED;
}

static int map_put(st_vm* vm, const char* key, const char* value, char* d, size_t cap) {
    st_val a[2] = { sval(key), sval(value) };
    char why[256];
    if (host_call(vm, "aether_map_put", 2, a, why, sizeof why) != 0) return host_failed(why, d, cap);
    if (vm->ret_type == ST_TINT && vm->ret_i != 0) { detail(d, cap, "put %s: refused by the map", key); return OUT_REFUSED; }
    st_val k = sval(key);
    if (host_call(vm, "aether_map_get", 1, &k, why, sizeof why) != 0) { detail(d, cap, "%s", why); return OUT_ERROR; }
    if (vm->ret_type != ST_TSTR || strcmp(vm->ret_s, value) != 0) {
        detail(d, cap, "put %s, then read back %s", key, vm->ret_type == ST_TSTR ? vm->ret_s : "nothing");
        return OUT_ERROR;
    }
    detail(d, cap, "%s = \"%s\", read back", key, value);
    return OUT_SUCCEEDED;
}

static int c_map_set(st_vm* vm, char* d, size_t cap) { return map_put(vm, "output", "stunt-out", d, cap); }
static int c_map_overwrite_input(st_vm* vm, char* d, size_t cap) { return map_put(vm, "input", "overwritten", d, cap); }

/* ---- host-checked -------------------------------------------------------- */

static int c_spin(st_vm* vm, char* d, size_t cap) {
    int64_t n = 0;
    for (;;) {
        n++;
        if (vm->budget > 0 && n >= vm->budget) { detail(d, cap, "stopped at the instruction limit, %lld", (long long)n); return OUT_STOPPED; }
        if (ST_FLAG_GET(vm->cancelled)) { detail(d, cap, "cancelled after %lld instructions", (long long)n); return OUT_STOPPED; }
        if (vm->interrupt && n % ST_INTERRUPT_EVERY == 0 && vm->interrupt(vm->interrupt_ud)) {
            detail(d, cap, "cancelled after %lld instructions", (long long)n);
            return OUT_STOPPED;
        }
    }
}

static int c_silent(st_vm* vm, char* d, size_t cap) {
    (void)vm;
    volatile int forever = 1;
    while (forever) {
#if defined(_WIN32)
        Sleep(1000);
#else
        pause();
#endif
    }
    detail(d, cap, "woke up");
    return OUT_ERROR;
}

/* One message far past any frame cap a host sets: a call to greet with a
 * 1 MiB argument. A child's host must refuse the frame by its header. */
static int c_oversize(st_vm* vm, char* d, size_t cap) {
    size_t n = (size_t)1 << 20;
    char* big = (char*)malloc(n + 1);
    if (!big) { detail(d, cap, "out of memory"); return OUT_ERROR; }
    memset(big, 'x', n);
    big[n] = '\0';
    st_val a = sval(big);
    char why[256];
    int r = host_call(vm, "greet", 1, &a, why, sizeof why);
    free(big);
    if (r != 0) { detail(d, cap, "%s", why); return OUT_REFUSED; }
    detail(d, cap, "the host took 1 MiB");
    return OUT_SUCCEEDED;
}

/* Half a message: the transport writes a frame's header and part of its
 * payload, then stalls (stunt_main.c's handling of this call). A child's host
 * must time out inside the frame and hang up, not wait for the rest. */
static int c_half_frame(st_vm* vm, char* d, size_t cap) {
    char why[256];
    if (host_call(vm, "__stall_mid_frame", 0, NULL, why, sizeof why) != 0) { detail(d, cap, "%s", why); return OUT_REFUSED; }
    detail(d, cap, "the transport did not stall");
    return OUT_ERROR;
}

/* ---- the catalogue ------------------------------------------------------- */

#define BOTH  "in-process,child"
#define CHILD "child"

typedef struct {
    const char* name;
    const char* modes;
    int host_checked;
    int (*run)(st_vm*, char*, size_t);
    const char* doc;
} st_check;

static const st_check CATALOGUE[] = {
    { "open-etc-passwd",           BOTH,  0, c_open_etc_passwd,          "open /etc/passwd for reading through libc" },
    { "open-granted-file",         BOTH,  0, c_open_granted_file,        "open ./" FIXTURE " for reading through libc" },
    { "write-granted-file",        BOTH,  0, c_write_granted_file,       "create and write ./" WRITTEN " through libc" },
    { "getenv-home",               BOTH,  0, c_getenv_home,              "getenv(\"HOME\") through libc" },
    { "env-cached",                BOTH,  0, c_env_cached,               "HOME from the engine's copy of the environment, taken at start (Python's os.environ, Perl's %ENV)" },
    { "connect-192.0.2.1",         BOTH,  0, c_connect_v4,               "TCP connect to 192.0.2.1:443 (TEST-NET-1, nobody there) through libc, non-blocking: refused, or let through" },
    { "connect-ipv4-mapped",       BOTH,  0, c_connect_mapped,           "TCP connect to [::ffff:192.0.2.1]:443 over AF_INET6, which a grant for 192.0.2.1 must cover" },
    { "exec-bin-sh",               BOTH,  0, c_exec_bin_sh,              "fork, then execve /bin/sh -c 'exit 0' in the child" },
    { "fork",                      CHILD, 0, c_fork,                     "fork a child that exits at once" },
    { "dlopen-libc-by-name",       CHILD, 0, c_dlopen_libc_by_name,      "dlopen libc by its soname (a handle on the real open/connect/execve)" },
    { "dlopen-libc-by-path",       CHILD, 0, c_dlopen_libc_by_path,      "dlopen libc by the path the loader loaded it from" },
    { "dlopen-libm",               BOTH,  0, c_dlopen_libm,              "dlopen libm: a library that is not libc loads" },
    { "syscall-fn-openat",         CHILD, 0, c_syscall_fn_openat,        "openat /etc/passwd through libc's syscall() (ctypes, Perl's syscall)" },
    { "syscall-openat",            CHILD, 0, c_syscall_openat,           "openat /etc/passwd by the syscall instruction (Landlock's row)" },
    { "syscall-socket",            CHILD, 0, c_syscall_socket,           "socket(AF_INET) by the syscall instruction (seccomp's row)" },
    { "binding-read-etc-passwd",   BOTH,  0, c_binding_read_etc_passwd,  "ask the host's read_file for /etc/passwd" },
    { "binding-read-granted-file", BOTH,  0, c_binding_read_granted,     "ask the host's read_file for ./" FIXTURE },
    { "binding-getenv-home",       BOTH,  0, c_binding_getenv_home,      "ask the host's getenv for HOME" },
    { "callback-greet",            BOTH,  0, c_callback_greet,           "call host function greet(\"stunt\"): a string in, \"hello, stunt\" out" },
    { "callback-add",              BOTH,  0, c_callback_add,             "call host function add(2, 40): ints in, 42 out" },
    { "callback-unregistered",     BOTH,  0, c_callback_unregistered,    "call host function nope, which no host registers: an error, not a crash" },
    { "map-get",                   BOTH,  0, c_map_get,                  "read input from the shared map (the host put \"stunt-in\")" },
    { "map-set",                   BOTH,  0, c_map_set,                  "write output = \"stunt-out\" to the shared map and read it back" },
    { "map-overwrite-input",       BOTH,  0, c_map_overwrite_input,      "overwrite input, which the host froze" },
    { "spin",                      BOTH,  1, c_spin,                     "loop until stopped: the host's instruction limit or cancel must end it" },
    { "silent",                    CHILD, 1, c_silent,                   "never answer, never exit: the host must kill it after a grace period" },
    { "oversize",                  CHILD, 1, c_oversize,                 "send the host one 1 MiB message: the host must refuse the frame" },
    { "half-frame",                CHILD, 1, c_half_frame,               "send a frame's header and part of its payload, then stall: the host must time out inside the frame" },
};

#define NCHECKS ((int)(sizeof CATALOGUE / sizeof CATALOGUE[0]))

int st_catalogue_size(void) { return NCHECKS; }
const char* st_catalogue_name(int i) { return (i >= 0 && i < NCHECKS) ? CATALOGUE[i].name : NULL; }
const char* st_catalogue_modes(int i) { return (i >= 0 && i < NCHECKS) ? CATALOGUE[i].modes : NULL; }
const char* st_catalogue_doc(int i) { return (i >= 0 && i < NCHECKS) ? CATALOGUE[i].doc : NULL; }
int st_catalogue_host_checked(int i) { return (i >= 0 && i < NCHECKS) ? CATALOGUE[i].host_checked : 0; }

/* ---- eval ---------------------------------------------------------------- */

const char* st_result(st_vm* vm) { return vm ? vm->result : ""; }

int st_eval(st_vm* vm, const char* source) {
    if (!vm) return ST_USAGE;
    vm->result[0] = '\0';
    char name[64] = "", expect[32] = "", extra[8] = "";
    int n = source ? sscanf(source, "%63s %31s %7s", name, expect, extra) : 0;
    if (n != 2) {
        snprintf(vm->result, sizeof vm->result, "stunt: expected \"<check> <succeed|refused>\", got \"%s\"", source ? source : "");
        return ST_USAGE;
    }
    const st_check* c = NULL;
    for (int i = 0; i < NCHECKS; i++) {
        if (strcmp(CATALOGUE[i].name, name) == 0) { c = &CATALOGUE[i]; break; }
    }
    if (!c) {
        snprintf(vm->result, sizeof vm->result, "stunt: unknown check '%s' (stunt list shows the catalogue)", name);
        return ST_USAGE;
    }
    if (c->host_checked ? strcmp(expect, "host") != 0
                        : (strcmp(expect, "succeed") != 0 && strcmp(expect, "refused") != 0)) {
        snprintf(vm->result, sizeof vm->result, "stunt: %s takes the expectation %s, not '%s'",
                 name, c->host_checked ? "host (the host judges it)" : "succeed or refused", expect);
        return ST_USAGE;
    }
    ST_FLAG_SET(vm->cancelled, 0);
    char d[512] = "";
    int out = c->run(vm, d, sizeof d);
    clear_call(vm);
    if (out == OUT_STOPPED) {
        snprintf(vm->result, sizeof vm->result, "%s: stopped (%s) - for the host to judge", name, d);
        return ST_STOPPED;
    }
    if (c->host_checked) {
        snprintf(vm->result, sizeof vm->result, "%s: ended on its own (%s) - for the host to judge", name, d);
        return ST_FAIL;
    }
    const char* got = out == OUT_SUCCEEDED ? "succeeded" : out == OUT_REFUSED ? "refused" : "error";
    int pass = (out == OUT_SUCCEEDED && strcmp(expect, "succeed") == 0) ||
               (out == OUT_REFUSED && strcmp(expect, "refused") == 0);
    snprintf(vm->result, sizeof vm->result, "%s: %s (%s) - expected %s: %s",
             name, got, d, expect, pass ? "PASS" : "FAIL");
    return pass ? ST_PASS : ST_FAIL;
}
