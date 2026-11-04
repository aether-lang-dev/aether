#ifndef PARSE_LIMITS_H
#define PARSE_LIMITS_H

/* How deep and how long a program the parser takes. In a header of its own
 * (#3574) so that the `ae` driver, which hands the generated C to a C
 * compiler, can size what that compiler must take from it without the
 * parser's types. */

// The deepest real nesting in this repository is 22 braces and 9 parens, so
// this is roughly twenty times what hand-written code reaches, and a quarter
// of the shallowest measured crash. Frames here are small, but a thread with a
// 512 KB stack must survive the limit too, which is why it is not raised to
// meet the crash point.
#define AETHER_MAX_PARSE_DEPTH 512

// #2907: operators chained at one level (`a + b + c ...`) and postfix
// operations on one operand (`a.b.c ...`, `x[i][j] ...`) are parsed by a loop,
// not by recursion, but each one nests the tree a level deeper, and every
// later pass walks that tree recursively: 4000 `+ 1` overflowed aetherc's
// stack on Windows with no diagnostic. A chain longer than this is E0100,
// naming the limit, and parsing stops as at AETHER_MAX_PARSE_DEPTH. It is
// the old silent cap on binary operators, now reported; nothing else in the
// parser counts (declarations, statements, cases and arms are unlimited).
#define AETHER_MAX_EXPR_CHAIN 1000

/* #3574: the bracket depth the C of any program the parser takes stays
 * within. An expression's C nests by a constant (codegen_mark_depth, the
 * flat chains), but blocks nest as the source does, and a level of the
 * parser's depth is at most two brackets of C (a `match` arm 508 deep is
 * 1018), so four a level leaves room for the expression inside the deepest
 * block. The `ae` driver hands it to a C compiler with a fixed limit it can
 * raise (Clang's -fbracket-depth, 256 by default). */
#define AETHER_C_BRACKET_DEPTH 2048

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(AETHER_C_BRACKET_DEPTH >= 4 * AETHER_MAX_PARSE_DEPTH,
               "the C bracket depth is sized from the parser's own depth");
#endif

#endif
