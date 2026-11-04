/* The compiler's own stack (#3539): aetherc and aether-lsp run their entry
 * on a thread whose stack the compiler sizes, started with the caller's
 * floating point control. */
#include "../runtime/test_harness.h"
#include "../../compiler/aether_compile_stack.h"
#include <string.h>

static int entry_echo(int argc, char** argv) {
    return argc * 100 + (argv && argv[1] ? (int)strlen(argv[1]) : 0);
}

TEST_CATEGORY(compile_stack_runs_the_entry_with_its_arguments, TEST_CATEGORY_COMPILER) {
    char a0[] = "aetherc";
    char a1[] = "hello";
    char* argv[] = { a0, a1, NULL };
    ASSERT_EQ(205, aether_run_on_compile_stack(entry_echo, 2, argv));
}

/* About 32 MB of frames, four times the 8 MB a main thread gets by default
 * on Linux and macOS and 32 times Windows' 1 MB: only a stack of the
 * compiler's own holds it. */
static int deep(int n) {
    volatile char frame[1024];
    frame[0] = (char)n;
    frame[sizeof frame - 1] = (char)n;
    if (n == 0) return frame[0];
    return deep(n - 1) + (frame[sizeof frame - 1] == (char)n ? 1 : 0);
}

static int entry_deep(int argc, char** argv) {
    (void)argc;
    (void)argv;
    return deep(32 * 1024);
}

TEST_CATEGORY(compile_stack_holds_a_deep_recursion, TEST_CATEGORY_COMPILER) {
    char a0[] = "aetherc";
    char* argv[] = { a0, NULL };
    ASSERT_EQ(32 * 1024, aether_run_on_compile_stack(entry_deep, 1, argv));
}

#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
static unsigned short x87_cw(void) {
    unsigned short cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    return cw;
}

static int entry_x87(int argc, char** argv) {
    (void)argc;
    (void)argv;
    return (int)x87_cw();
}

/* MinGW starts the main thread's x87 at 64 bit precision and Windows every
 * other thread at 53: the compile thread keeps the caller's, so a long
 * double the compiler computes (a duration literal) is the same as it was
 * on the main thread. Set to something no thread starts with, to tell a
 * copy from a default, and compared with what the caller then reads back:
 * Valgrind keeps the rounding it is given but reports 64 bit precision
 * whatever was set. */
TEST_CATEGORY(compile_stack_keeps_the_callers_x87_control, TEST_CATEGORY_COMPILER) {
    unsigned short saved = x87_cw();
    unsigned short want = (unsigned short)((saved & ~0x0300u) | 0x0200u);  /* 53 bit precision */
    want = (unsigned short)((want & ~0x0C00u) | 0x0400u);                  /* round down */
    __asm__ volatile("fldcw %0" : : "m"(want));
    unsigned short callers = x87_cw();
    char a0[] = "aetherc";
    char* argv[] = { a0, NULL };
    int got = aether_run_on_compile_stack(entry_x87, 1, argv);
    __asm__ volatile("fldcw %0" : : "m"(saved));
    ASSERT_TRUE((callers & 0x0C00u) == 0x0400u);   /* the rounding took, so a default is told apart */
    ASSERT_EQ((int)callers, got);
}
#endif
