#ifndef AETHER_STRINGSEQ_H
#define AETHER_STRINGSEQ_H

#include <stddef.h>

/* std.collections.string_seq — Erlang/Elixir-shaped cons-cell linked
 * list of string values.
 *
 * Empty list is the NULL pointer. Each cell carries a CACHED length
 * field so `string_seq_length(s)` is O(1) — Erlang's BEAM doesn't
 * cache length per cell because of its fixed compact cell layout;
 * Aether controls the layout, so we do.
 *
 * Cells are reference-counted to allow structural sharing without
 * copying:
 *
 *     t  = string_seq_cons("c",  string_seq_cons("d", NULL));
 *     a  = string_seq_cons("a",  t);   // shares t
 *     b  = string_seq_cons("b",  t);   // shares t
 *     string_seq_free(a);              // a frees, t stays alive for b
 *     string_seq_free(b);              // b frees, t now drops to 0 and frees
 *
 * Cells are also IMMUTABLE after creation — `cons` is the only
 * constructor; there's no `set_head` or `set_tail`. That makes
 * cycles impossible, so the iterative free below cannot loop, and
 * we don't need cycle detection on the spine.
 *
 * Operation complexity:
 *   empty / cons / head / tail / is_empty / length / retain : O(1)
 *   free                                                    : O(n) work,
 *                                                              O(1) stack
 *                                                              (iterative)
 *   from_array                                              : O(n)
 *   to_array                                                : O(n)
 *
 * Refcount semantics:
 *   - `cons(h, t)` retains BOTH `h` (via aether_string_retain) and
 *     `t` (via string_seq_retain). Callers that want to transfer
 *     their ref into the new cell call `string_seq_free` on the old
 *     local immediately afterward — see `string_seq_from_array` for
 *     the canonical builder pattern.
 *   - `retain(s)` bumps a cell's refcount; pair with `free(s)`.
 *   - `free(s)` decrements; if it reaches zero we release the head
 *     and walk to the tail. If a tail cell is still shared
 *     (refcount > 0 after decrement) we stop — the other owner
 *     finishes the walk later when its own free runs.
 *
 * Issue: motivated by the svn-aether port hitting a `string.split()`-
 * returning-`ptr` mismatch with `string[]` message fields. Cons cells
 * give a runtime-built sequence shape that fits actor messages
 * cleanly without forcing callers to plumb a separate `count` field.
 */

typedef struct StringSeq {
    int   ref_count;
    int   length;            /* length of THIS list (head + length(tail)). */
    void* head;              /* AetherString* or plain const char* (refcount-aware via str_*). */
    struct StringSeq* tail;  /* NULL = end of list. */
} StringSeq;

/* Empty list. Returns NULL — we treat NULL itself as the empty seq.
 * Callers shouldn't depend on a unique sentinel pointer; treat the
 * return value as opaque. */
StringSeq* string_seq_empty(void);

/* Prepend `head` to `tail`. Retains `head` and `tail`. Returns a new
 * cell with refcount = 1 and `length = (tail ? tail->length : 0) + 1`.
 * NULL on OOM. Callers transferring ownership of `tail` into the new
 * cell should call `string_seq_free(tail)` after this returns to drop
 * the local ref — the new cell holds its own. */
StringSeq* string_seq_cons(const char* head, StringSeq* tail);

/* Borrowed reference to the head element. Returns "" on the empty
 * list (matches the existing `string.length("")` shape — caller can
 * treat the empty case identically). The returned pointer is
 * borrowed: callers that need to outlive the seq must retain or copy.
 *
 * Returns `const char*` (not `const void*`) so the C signature lines
 * up with what Aether emits for `-> string` returns. The byte
 * pointer behind it can be either an AetherString* header (the
 * magic-tagged refcounted shape) or a plain char* literal —
 * downstream code dispatches via the standard `is_aether_string`
 * magic check. Callers that want the unwrapped payload bytes call
 * `aether_string_data` on the result. */
const char* string_seq_head(StringSeq* s);

/* Borrowed pointer to the tail seq. NULL on empty. The returned
 * pointer's lifetime is tied to the parent `s`; callers that need
 * to retain it independently must call `string_seq_retain`. */
StringSeq* string_seq_tail(StringSeq* s);

/* 1 if empty (NULL), 0 otherwise. */
int string_seq_is_empty(StringSeq* s);

/* O(1) cached length. 0 on empty. */
int string_seq_length(StringSeq* s);

/* Bump refcount. NULL-safe. Returns the same pointer for chaining. */
StringSeq* string_seq_retain(StringSeq* s);

/* Decrement refcount; if it reaches 0, release head and recurse into
 * tail (iteratively — no stack growth for deep lists). Stops at the
 * first cell whose refcount stays > 0 after decrement (shared tail
 * still owned elsewhere). NULL-safe and idempotent on a fully-freed
 * spine. */
void string_seq_free(StringSeq* s);

/* Build a seq from an AetherStringArray* (the shape `string.split`
 * returns). Each element of `arr->strings` becomes one cell. The
 * caller supplies `count` so a longer array can be sub-sequenced
 * via the same entry point. Returns NULL on empty input or OOM
 * (any partial spine is freed before return).
 *
 * The signature uses bare `void*` rather than `AetherStringArray*`
 * so callers can write the Aether-side declaration as `arr: ptr`
 * without a cross-module cast — the function knows the shape
 * internally. */
StringSeq* string_seq_from_array(void* arr, int count);

/* Materialise `s` as an AetherStringArray* (the legacy split result
 * shape). Useful for interop with code that already consumes the
 * AetherStringArray surface. Caller frees with `string_array_free`.
 * Returns NULL on empty input or OOM. */
void* string_seq_to_array(StringSeq* s);

/* ---- Closure-free combinators ---------------------------------------
 *
 * The shape of these deliberately avoids callbacks — Aether's
 * function-typed-parameter / closure FFI bridge is a separate design
 * question (tracked under "closure-bearing combinators" in the PR
 * out-of-scope list). The four below are pure structural ops over
 * the cons cells: reverse the spine, append two seqs, take the
 * first n cells, drop the first n cells. All retain refs correctly
 * for shared tails. NULL-safe. Return NULL on OOM, with any
 * partial spine freed before return.
 */

/* Returns a new seq with the elements of `s` in reverse order. The
 * result is independently allocated; `s` is left untouched. O(n)
 * work, O(1) auxiliary stack. NULL on empty input or OOM. */
StringSeq* string_seq_reverse(StringSeq* s);

/* Returns a new seq that is the elements of `a` followed by `b`.
 * The cells from `a` are *copied* (one new cell per element); the
 * tail of the new seq points at `b` directly with a shared
 * reference, so freeing the result drops one ref from `b` but
 * leaves `b` walkable from the caller's other handles. O(|a|)
 * work — `b` is shared, not walked. NULL on OOM. */
StringSeq* string_seq_concat(StringSeq* a, StringSeq* b);

/* Join elements with `sep` between adjacent pairs. Linear cost (two
 * passes, one exact-size allocation); the escape from the O(n^2)
 * interpolation-self-append accumulation trap. Returns a fresh
 * caller-owned AetherString (returned as const char*, matching the
 * string_seq_head convention and the generated extern declaration);
 * "" for the empty seq. The separator accepts both AetherString* and
 * plain char* (magic-dispatched). */
const char* string_seq_join(StringSeq* s, const char* sep);

/* Returns a new seq with the first min(n, length(s)) elements of
 * `s`. New cells; the tail is NULL (independent spine, not shared
 * with `s`). n <= 0 returns empty. O(min(n, length)) work. */
StringSeq* string_seq_take(StringSeq* s, int n);

/* Returns the n-th tail of `s` — i.e. the seq obtained by skipping
 * the first n elements. Increments the resulting cell's refcount
 * so the returned handle is independent (caller must free). n
 * exceeding length returns the empty seq (NULL). n <= 0 returns
 * `s` retained. O(min(n, length)) work — pointer walk only, no
 * new allocations. */
StringSeq* string_seq_drop(StringSeq* s, int n);

/* ---- Closure-bearing combinators ------------------------------------
 *
 * Higher-order iteration over the cons spine, taking an Aether closure
 * as the per-element callback (issue #421 — multi-sequence iteration
 * primitives). These resolve the "deferred to a follow-up change set
 * once the Aether-callback-from-C bridge is settled" note that the
 * structural combinators above carried.
 *
 * Closure ABI
 * -----------
 * Each callback parameter is declared `ptr` (C `void*`) on both sides,
 * and the Aether `extern` declares it `ptr` too. When a caller passes
 * a closure literal into a `ptr`-typed slot, the codegen heap-BOXES it:
 *
 *     extern_fn(..., _aether_box_closure(
 *         (_AeClosure){ .fn = (void(*)(void))_closure_fn_N, .env = _e }))
 *
 * where `_AeClosure` is the codegen prologue's
 *
 *     typedef struct { void (*fn)(void); void* env; } _AeClosure;
 *
 * `_aether_box_closure` malloc's a copy of that two-word struct and
 * hands us the heap pointer. We mirror the layout locally as
 * `AeSeqClosure` (see the .c — this TU need not see the prologue
 * typedef) and read `.fn` / `.env` back out. The closure body is then
 * invoked as
 *
 *     ((RET(*)(void* env, ARG1, ARG2, ...))clo.fn)(clo.env, a1, a2, ...)
 *
 * i.e. `env` (the captured environment, NULL for a non-capturing
 * closure) is ALWAYS the implicit first argument, ahead of the declared
 * parameters — exactly what codegen emits for `call(cb, ...)` and for
 * the `_closure_fn_N(env, ...)` definition.
 *
 * Ownership: the box (and the `env` it points at, when non-NULL) is
 * malloc'd by `_aether_box_closure` and OWNED BY THE CALLEE — the same
 * contract `list_add_closure_owned` / `list_free` use in
 * aether_collections.c. Each combinator below frees the env then the
 * box before returning, so a one-shot `string.seq_map(s, |x| ...)` does
 * not leak the closure.
 *
 * Complexity: every combinator is a single iterative spine walk —
 * O(n) time, O(1) auxiliary stack (no recursion). `map`/`filter`
 * build a fresh, independently-owned result spine (free with
 * `string_seq_free`); `each`/`zip_each` return nothing; `reduce`
 * returns the caller's accumulator.
 */

/* Apply `f(elem)` to each element, head-to-tail, for side effects.
 * `f`'s return value (if any) is discarded. NULL-safe (empty seq is a
 * no-op). `s` is borrowed — not consumed, not freed. */
void string_seq_each(StringSeq* s, void* f);

/* Return a NEW seq with `f(elem)` substituted for each element, order
 * preserved. `f` must return a `string` (its result is consed into the
 * fresh spine, which retains it). The result is caller-owned: free with
 * `string_seq_free`. `s` is left untouched. NULL on OOM (any partial
 * result spine is freed before return). O(n). */
StringSeq* string_seq_map(StringSeq* s, void* f);

/* Return a NEW seq of the elements for which `pred(elem)` is truthy
 * (non-zero), order preserved. The result is caller-owned: free with
 * `string_seq_free`. `s` is left untouched. NULL on empty result or
 * OOM (any partial result spine is freed before return). O(n). */
StringSeq* string_seq_filter(StringSeq* s, void* pred);

/* Left fold: starting from `init`, compute `acc = f(acc, elem)` for
 * each element head-to-tail, and return the final `acc`. `acc` and the
 * return are opaque `ptr`s — the closure owns whatever it threads
 * through. NULL-safe (empty seq returns `init` unchanged). O(n). */
void* string_seq_reduce(StringSeq* s, void* init, void* f);

/* Implicit-zip iteration over two seqs: call `f(a_elem, b_elem)` for
 * each aligned pair, stopping at the end of the SHORTER seq. Covers
 * issue #421's multi-sequence intent without dynamic typing. Both
 * seqs are borrowed. NULL-safe (a NULL either side ends immediately).
 * O(min(|a|, |b|)). */
void string_seq_zip_each(StringSeq* a, StringSeq* b, void* f);

#endif
