/* Slice ownership: a buffer `make([]T, n)` hands out, or `append` or `push`
 * grows, is counted (R1, #3568), and every place that holds a slice of it
 * holds one reference (R2): the last place to give its reference back
 * gives back the buffer and what each element holds.
 *
 * A slice local holds a reference: one a call hands over (`xs = f()`, `xs =
 * make(...)`), one moved out of a local that owns (`ys = xs`), or a copy of
 * its own of any other slice (`v = xs[1..3]`, `v = p` of a parameter, `v =
 * s.items`, `v = rows[i]`). It gives the reference back on every exit of its
 * scope (the end of its block, a `return`, a `break` or `continue`, a
 * caught panic), by a `defer` this pass puts after its binding, and a store
 * into it (`xs = v`) gives back what it held. So a view keeps the buffer it
 * views for as long as it is held, wherever its owner went: bound in an
 * outer scope, held while the owner is bound again, returned, stored.
 *
 * A local that owns what it holds moves: binding it to another name (`ys =
 * xs`), returning it, storing it into a field, a module-level `var` or an
 * element, putting it in a struct, a tuple, an array or a message, and
 * giving it to a `@keep` parameter each take it out of the local, which is
 * left empty (and its release finds nothing). Using the local again before
 * it is bound anew is an error (E0906), as it is after `free(xs)` and after
 * `append(xs, v)`, which consume it. A local only ever given views and
 * copies is copied instead, a reference of its own for each place. A
 * parameter borrows: it holds no reference, and `free`, `append` and `push`
 * on it are refused (E0901) unless it is declared `xs: @keep T[]`, when the
 * caller's reference moves into it and it is released as a local that
 * owns. Passing a slice to any other parameter, indexing it, measuring it,
 * viewing it (`xs[a..b]`, `xs[..]`) and iterating it borrow it.
 *
 * A copy (aether_slice_copy) is marked a view: it shares the buffer, so
 * growing it is a named panic. `free` of any place gives back the
 * reference it holds and leaves the place empty. A store into a field, an
 * element, a module-level `var` or an actor's state field gives back what
 * the place held (`__slice_set`, `__slice_set_shared` for a place other
 * threads read, which publishes the stored buffer).
 *
 * A slice local a closure assigns is a cell the frame and its closures
 * share (codegen promotes it, by name): the cell holds the reference. Every
 * store into it, in the frame or a closure, gives back what it held, and
 * the cell's last release gives back what it holds then (#3549). A closure
 * that only reads a slice holds a copy of it in its environment.
 *
 * A fresh buffer given straight to a call (`total(make([]int, 3))`) is the
 * call's argument temporary, released once the call returns (the
 * `slice_temp` marker): a callee that keeps it takes a reference of its
 * own. No rule reads another function's body.
 *
 * A local a pattern, a destructure or a `catch` binds holds what it was
 * bound from, as before: what it holds stays with that value.
 *
 * The pass runs over the type-checked program (before the escape check,
 * slice_escape.c, which keeps a view of the frame's own arrays in the
 * frame) and rewrites it for codegen with the intrinsics `__slice_take`,
 * `__slice_copy`, `__slice_lend`, `__slice_drop`, `__slice_set` and
 * `__slice_set_shared`, and the marker `slice_temp`. Returns the number of
 * errors reported. */

#ifndef AETHER_SLICE_OWN_H
#define AETHER_SLICE_OWN_H

#include "../ast.h"

int check_slice_ownership(ASTNode* program);

#endif
