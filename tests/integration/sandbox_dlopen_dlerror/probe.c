/* Run under the preload with no native grant. Each refused dlopen must leave
 * a dlerror() string naming what was refused, returned once and then
 * cleared, as dlopen's contract has it: Tcl 8.6's `load` prints dlerror()
 * with %s and crashed on the NULL a refusal used to leave. Prints one line
 * per check, "<label> ok" or "<label> BAD <why>". */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

static void report(const char* label, int ok, const char* why) {
    if (ok) printf("%s ok\n", label);
    else printf("%s BAD %s\n", label, why ? why : "(null)");
    fflush(stdout);
}

/* dlopen `name`, which the sandbox must refuse, then read dlerror twice. */
static void refused(const char* label, const char* name, const char* expect) {
    char l[64];
    void* h = dlopen(name, RTLD_NOW);
    snprintf(l, sizeof l, "%s-null", label);
    report(l, h == NULL, "the load was not refused");
    if (h) { dlclose(h); return; }
    const char* e = dlerror();
    snprintf(l, sizeof l, "%s-message", label);
    report(l, e && strstr(e, expect) && strstr(e, "sandbox"), e);
    const char* again = dlerror();
    snprintf(l, sizeof l, "%s-cleared", label);
    report(l, again == NULL, again);
}

static void* other_thread(void* arg) {
    (void)arg;
    const char* e = dlerror();
    report("thread-local", e == NULL, e);
    return NULL;
}

int main(int argc, char** argv) {
    const char* tmp = argc > 1 ? argv[1] : "/tmp";

    /* Refused by name. */
    refused("bare", "libc.so.6", "libc.so.6");

    /* Refused by handle: an innocent name the loader resolves to libc. The
     * script puts the symlink in `tmp` and `tmp` on LD_LIBRARY_PATH. */
    refused("handle", "libplain.so", "libplain.so");

    /* A refusal does not shadow the loader's own later error. */
    char missing[PATH_MAX];
    snprintf(missing, sizeof missing, "%s/no-such-library.so", tmp);
    (void)dlopen("libc.so.6", RTLD_NOW);
    void* h = dlopen(missing, RTLD_NOW);
    const char* e = dlerror();
    report("loader-error", h == NULL && e && strstr(e, "no-such-library.so") &&
           !strstr(e, "sandbox"), e);

    /* The message belongs to the thread that was refused. */
    (void)dlopen("libc.so.6", RTLD_NOW);
    pthread_t t;
    if (pthread_create(&t, NULL, other_thread, NULL) == 0) pthread_join(t, NULL);
    e = dlerror();
    report("own-thread", e && strstr(e, "libc.so.6"), e);

    /* Nothing refused: a library that is not libc loads, and no message. */
    h = dlopen("libm.so.6", RTLD_NOW);
    e = dlerror();
    report("libm", h != NULL && e == NULL, e);
    if (h) dlclose(h);
    return 0;
}
