/* Slice ownership: a buffer `make([]T, n)` hands out, or `append` or `push`
 * grows, has one owner, and is released when that owner goes out of scope.
 *
 * A slice local the program binds (`xs = make([]int, n)`, `int[] xs = []`,
 * `xs = f()`) owns what it holds when anything it is given may be such a
 * buffer: a call's result, a local that owns one, or a buffer `free`,
 * `append` or `push` is applied to. It is dropped on every exit of its
 * scope (the end of its block, a `return`, a `break` or `continue`, a
 * caught panic) by a `defer` this pass puts after its binding: the buffer,
 * and what each element holds, are released, unless the buffer was moved
 * out first. A local only ever given views (a sub-slice, an array, a
 * field, a parameter) owns nothing and is never dropped.
 *
 * A slice moves: binding it to another name (`ys = xs`), returning it,
 * storing it into a field, a module-level `var` or an element, putting it
 * in a struct, a tuple, an array or a message, and giving it to a `@keep`
 * parameter each take it out of the local, which is left empty (and its
 * drop finds nothing). Using the local again before it is bound anew is an
 * error (E0906), as it is after `free(xs)` and after `append(xs, v)`, which
 * consume it. A parameter borrows: it is never dropped, and `free`,
 * `append` and `push` on it are refused (E0901) unless it is declared
 * `xs: @keep T[]`, when the caller's buffer moves into it and it is
 * dropped as a local that owns. Passing a slice to any other parameter,
 * indexing it, measuring it, viewing it (`xs[a..b]`, `xs[..]`) and
 * iterating it borrow it.
 *
 * A copy of a slice out of a place that does not own it for the copy (a
 * parameter, a field, an element, a module-level `var`, a local captured
 * by a closure or bound by a pattern) is lent: it is marked a view at run
 * time (aether_slice_lend), so the place that holds the copy never frees
 * it. An element of a slice of slices owns its row: a store into it drops
 * the row it replaces, and the outer slice's drop (or `free`) drops every
 * row.
 *
 * A view of a local's buffer that may outlive the local (returned, stored,
 * given to a parameter its function stores or returns, captured by a
 * closure that escapes, bound in an outer scope, or held while the local is
 * re-bound, moved, freed or grown) keeps the buffer: the local does not
 * release it, as nothing did before ownership (the escape analysis in
 * slice_own.c, with a summary per function of what it does with its slice
 * parameters, to a fixed point over the calls).
 *
 * A slice local a closure assigns is a cell the frame and its closures
 * share (codegen promotes it), and the cell owns what it holds (#3549)
 * where nothing else can reach the buffer: every use of the local in the
 * frame borrows it, stores into it or returns it, with no view of its
 * buffer bound or escaping, and every closure that captures it is bound to
 * a local only called and only stores into it, indexes it or reads its
 * length. Then every store into it, in the frame or the closure, drops
 * what it held (`__slice_set`), the cell's last release drops what it
 * holds then, and a `return` of it gives the buffer to the caller and
 * leaves the cell a loan (`__slice_give`). A closure holds the cell it
 * stores into, not a view of the buffer in it.
 *
 * Left to the program, as before: any other slice local a closure
 * captures, or whose address is taken, or that a pattern or a destructure
 * binds, and the buffers held by fields, module-level `var`s, tuples and
 * messages.
 *
 * The pass runs over the type-checked program (before the escape check,
 * slice_escape.c, which counts a view of a buffer a local owns as the
 * frame's own storage) and rewrites it for codegen with the intrinsics
 * `__slice_take`, `__slice_lend`, `__slice_drop`, `__slice_set` and
 * `__slice_give`, and the markers `slice_temp` (a buffer made for a call,
 * which the call drops) and `slice_cell` (a cell owner's declaration).
 * Returns the number of errors reported. */

#ifndef AETHER_SLICE_OWN_H
#define AETHER_SLICE_OWN_H

#include "../ast.h"

int check_slice_ownership(ASTNode* program);

/* Does the identifier `n` name a local or a cell (#3549) that owns its
 * buffer (as the last check_slice_ownership found)? A view of that buffer
 * is storage of the frame, which the escape check keeps in it. */
int slice_own_owner_ident(const ASTNode* n);

#endif
