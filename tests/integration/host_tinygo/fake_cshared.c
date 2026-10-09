/* A stand-in, in C, for a `go build -buildmode=c-shared` library, so the
 * string-ownership half of contrib.host.tinygo is tested where no Go
 * toolchain is installed (#2569).
 *
 * Each export has the C signature cgo generates for the matching Go function
 * (`func F(a *C.char) *C.char` is `char* F(char*)`), and returns its string the
 * way `C.CString` does: copied into memory from malloc, for the caller to
 * free. Static returns a pointer the library keeps, the case the borrowed
 * wrappers are for, and Nil returns what a Go function returning nil does. */
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

/* a, sep and b in one malloc'd string, as C.CString would hand it back. */
static char* concat3(const char* a, const char* sep, const char* b) {
    size_t na = strlen(a), ns = strlen(sep), nb = strlen(b);
    char* p = malloc(na + ns + nb + 1);
    if (!p) return NULL;
    memcpy(p, a, na);
    memcpy(p + na, sep, ns);
    memcpy(p + na + ns, b, nb + 1);
    return p;
}

/* n copies of c. */
static char* run_of(char c, size_t n) {
    char* p = malloc(n + 1);
    if (!p) return NULL;
    memset(p, c, n);
    p[n] = '\0';
    return p;
}

EXPORT char* Banner(void) { return concat3("owned", " ", "banner"); }

EXPORT char* Greet(char* name) { return concat3("hello,", " ", name); }

EXPORT char* Stars(int n) { return run_of('*', n > 0 ? (size_t)n : 0); }

EXPORT char* Join(char* a, char* b) { return concat3(a, "-", b); }

EXPORT char* Join3(char* a, char* b, char* c) {
    char* ab = concat3(a, "-", b);
    if (!ab) return NULL;
    char* abc = concat3(ab, "-", c);
    free(ab);
    return abc;
}

EXPORT char* Nil(void) { return NULL; }

EXPORT const char* Static(void) { return "static banner"; }

/* kb KiB of 'a', for the heap check. */
EXPORT char* Big(int kb) { return run_of('a', kb > 0 ? (size_t)kb * 1024 : 0); }
