/* The compiler's own stack (#3539); see aether_compile_stack.h. */

#include "aether_compile_stack.h"

#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#  include <windows.h>
#  include <process.h>
#else
#  include <pthread.h>
#endif

/* Reserved address space: only the pages a compile touches are committed.
 * The parser refuses an operator chain past 1000 (E0100), which takes a few
 * MB on an ordinary build and several times that under AddressSanitizer. */
#define AETHER_COMPILE_STACK_BYTES ((size_t)64 * 1024 * 1024)

#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
#  define AETHER_COMPILE_STACK_X87 1
#else
#  define AETHER_COMPILE_STACK_X87 0
#endif
#if AETHER_COMPILE_STACK_X87 && defined(__SSE__)
#  define AETHER_COMPILE_STACK_SSE 1
#else
#  define AETHER_COMPILE_STACK_SSE 0
#endif

typedef struct {
    int (*entry)(int, char**);
    int argc;
    char** argv;
    int rc;
#if AETHER_COMPILE_STACK_X87
    unsigned short x87_cw;
#endif
#if AETHER_COMPILE_STACK_SSE
    unsigned int mxcsr;
#endif
} CompileRun;

static void compile_run(CompileRun* r) {
#if AETHER_COMPILE_STACK_X87
    __asm__ volatile("fldcw %0" : : "m"(r->x87_cw));
#endif
#if AETHER_COMPILE_STACK_SSE
    __asm__ volatile("ldmxcsr %0" : : "m"(r->mxcsr));
#endif
    r->rc = r->entry(r->argc, r->argv);
}

#ifdef _WIN32
static unsigned __stdcall compile_thread(void* arg) {
    compile_run((CompileRun*)arg);
    return 0;
}
#else
static void* compile_thread(void* arg) {
    compile_run((CompileRun*)arg);
    return NULL;
}
#endif

int aether_run_on_compile_stack(int (*entry)(int, char**), int argc, char** argv) {
    CompileRun r;
    r.entry = entry;
    r.argc = argc;
    r.argv = argv;
    r.rc = 1;
#if AETHER_COMPILE_STACK_X87
    __asm__ volatile("fnstcw %0" : "=m"(r.x87_cw));
#endif
#if AETHER_COMPILE_STACK_SSE
    __asm__ volatile("stmxcsr %0" : "=m"(r.mxcsr));
#endif
#ifdef _WIN32
    /* The size is the reservation, not the commit (CreateThread's flag,
     * which _beginthreadex passes through). */
    uintptr_t h = _beginthreadex(NULL, (unsigned)AETHER_COMPILE_STACK_BYTES, compile_thread, &r,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) return entry(argc, argv);
    WaitForSingleObject((HANDLE)h, INFINITE);
    CloseHandle((HANDLE)h);
    return r.rc;
#else
    pthread_attr_t attr;
    pthread_t t;
    if (pthread_attr_init(&attr) != 0) return entry(argc, argv);
    int made = pthread_attr_setstacksize(&attr, AETHER_COMPILE_STACK_BYTES) == 0 &&
               pthread_create(&t, &attr, compile_thread, &r) == 0;
    pthread_attr_destroy(&attr);
    if (!made) return entry(argc, argv);
    pthread_join(t, NULL);
    return r.rc;
#endif
}
