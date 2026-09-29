/* #1286 first-class slices: the representation coercions the typechecker
 * inserts into the AST so codegen never has to guess.
 *
 * An Aether `T[]` is a fat pointer, `{ ptr, len }` (AetherSlice in C). A
 * fixed `T[N]` array and a bare `ptr` are single C pointers. Where one
 * shape flows into a slot of the other, the typechecker wraps the
 * expression in an explicit coercion node:
 *
 *   `T[N]` / `null`  ->  `T[]` slot      AST_SLICE_FROM_ARRAY (len = N / 0)
 *   `T[]`            ->  raw-pointer slot AST_SLICE_TO_PTR    (`.ptr`)
 *
 * A raw-pointer slot is a `ptr`, or a `T[]` parameter of an extern C
 * function (C sees `T*` there), or an `extern struct` field. Codegen
 * lowers the two nodes and nothing else has to know about shapes.
 *
 * Every function is idempotent: an already-wrapped slot is left alone, so
 * the typechecker's repeated inference passes cannot double-wrap. */
#ifndef AETHER_SLICE_COERCE_H
#define AETHER_SLICE_COERCE_H

#include "../ast.h"

/* Wrap `*slot` if its value must change shape to land in a slot of type
 * `target`. `target_is_c_view` says the slot is a raw C pointer even when
 * `target` is spelled `T[]` (an extern parameter or extern-struct field).
 * Returns 1 when a coercion node was inserted, 0 otherwise. */
int slice_coerce_slot(ASTNode** slot, const Type* target, int target_is_c_view);

/* Wrap `*slot` in AST_SLICE_TO_PTR when it is slice-typed (pointer
 * arithmetic, a null / ptr comparison, `free`). Returns 1 if wrapped. */
int slice_coerce_to_ptr(ASTNode** slot);

/* Is `n` one of the two coercion nodes? */
int slice_is_coercion_node(const ASTNode* n);

#endif
