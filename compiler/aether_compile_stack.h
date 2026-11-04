/* The compiler's own stack (#3539).
 *
 * Every pass walks the tree recursively, and an expression nests as deep as
 * its operator chain is long, so how deep a program the compiler reads is
 * set by its stack. The main thread's stack is whatever the platform and the
 * user gave the process (8 MB by default on Linux and macOS, or whatever
 * `ulimit -s` says; 1 MB on Windows unless the binary asks), and
 * AddressSanitizer's larger frames overflowed it on the nesting the parser
 * accepts (tests/integration/parser_no_count_limits). The compiler's entry
 * points (aetherc, aether-lsp) run on a thread whose stack is the
 * compiler's to size. */
#ifndef AETHER_COMPILE_STACK_H
#define AETHER_COMPILE_STACK_H

/* entry(argc, argv) on a thread with the compiler's stack, its result
 * returned. The thread starts with the calling thread's floating point
 * control (MinGW starts the main thread's x87 at 64 bit precision, Windows
 * every other thread at 53, and the compiler reads duration literals in
 * long double). Where no such thread can be made, entry runs here. */
int aether_run_on_compile_stack(int (*entry)(int, char**), int argc, char** argv);

#endif /* AETHER_COMPILE_STACK_H */
