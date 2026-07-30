# Multi-Value Return Type Inference

> Post-mortem of bugs fixed in commit `bc42939` and its follow-up (commit
> `937b029`). The per-bug **Cause** sections describe pre-fix code that
> no longer exists; the **Fix** sections describe what landed. Read the
> "Putting it together" invariant and the litmus test as the durable
> takeaways, the bug-by-bug archaeology is frozen in time and will
> drift as `type_inference.c` evolves.

The Go-style `(value, err)` tuple return migration in stdlib (`std.map`,
`std.list`, `std.fs`, `std.zlib`, `std.cryptography`, …) put a lot of
weight on cross-function tuple-return inference. Several latent bugs
in `compiler/analysis/type_inference.c` only surface once a function's
body destructures a multi-value call and then itself returns a tuple
mixing a destructured local with a literal. This doc catalogs them and
describes the fix shape so future regressions in the same area are
recognizable.

For the broader inference flow (constraint collection, propagation,
phase ordering), see [`type-inference-guide.md`](type-inference-guide.md).
This doc covers only multi-value-return-specific edge cases.

## The shape that triggers the cluster

```aether
import std.fs
import std.cryptography

hash_file(p: string) {
    bytes, length, rerr = fs.read_binary(p)         // 3-tuple destructure
    if string.length(rerr) > 0 {
        return "", rerr                              // multi-value, mixes literal "" with destructured local
    }
    return cryptography.sha256_hex(bytes, length)    // single-value tuple-typed call
}
```

`hash_file` returns `(string, string)` in both branches. Pre-fix,
codegen emitted it as `_tuple_string_int` and every caller produced
`-Wint-conversion` errors. Four distinct bugs combined to produce that
result. Each is fixable in isolation; the test
`tests/integration/multi_return_destructure_chain/` exercises the
combination.

## Bug 1, cross-module local-var pollution

**Symptom.** Two modules each declare a local with the same name (e.g.
`mod_name`, one a string from a parameter assignment, the other a ptr
from a tuple destructure). Compiling a third module that imports both
fails with `error[E0200]: Type mismatch in variable initialization` on
an unrelated `target = mod_name` line.

**Cause.** `collect_function_constraints` added function-local symbols
to a flat global symbol table without scoping. Two functions' locals
collided at the table level, `lookup_symbol` returned whichever was
added more recently.

**Fix.** Snapshot `ctx->symbols->symbols` (the head of the linked list)
on entry, trim back to it on exit. Function-level symbols (function
defs, externs, imports) are added by other code paths before any
function body is visited, so they sit beneath the snapshot and are
unaffected.

**Status.** Shipped on `main` as commit `bc42939` ("type inference bug
fixed"). Test:
`tests/integration/tuple_destructure_cross_module/`.

## Bug 2, empty-string literal mis-typed as int

**Symptom.** A function ending in `return value, ""` (or with `""` in
any tuple slot) gets its return inferred as `(T, int)` instead of
`(T, string)`. Every caller that destructures the second slot then
fails to compile with pointer-from-int conversion errors.

**Cause.** The literal-type heuristic in
`infer_return_type_impl` (and the matching loop in
`merge_tuple_returns`) started `is_num = 1` and only flipped it to 0
on a non-numeric character. The empty buffer's loop body never runs,
so `is_num` stays 1, and the slot is typed as `TYPE_INT`.

**Fix.** Start `is_num = 0`; require at least one numeric character to
flip it to 1, and let any non-numeric character flip it back. Empty
string → `TYPE_STRING`, which is what every caller of the wrapper
already expects.

**Why it stayed latent.** Stdlib wrappers conventionally write
`return null, "literal-error-message"` for failure, never `return X,
""` because the success path uses an explicit non-empty string.
Downstream `aetherBuild`'s cache module is the first first-party
caller to hit this with `return "", rerr` (success means rerr is
empty; the caller needs the empty-string value as the meaningful
"no error" signal).

## Bug 3, partial tuple sticks across iterations

**Symptom.** A function whose return is correctly typed as
`TUPLE(string, UNKNOWN)` after iteration N never gets refined, even
though iteration N+1 has enough information to resolve the second
slot. Codegen emits the UNKNOWN slot as `int` (its "default", see
the `unresolved type in codegen, defaulting to int` warning).

**Cause.** `infer_function_return_types`'s gate to re-run inference
was:

```c
if (!node->node_type ||
    node->node_type->kind == TYPE_UNKNOWN ||
    node->node_type->kind == TYPE_VOID) { ... }
```

A function whose `node_type` was already a `TUPLE` (even a partially-
resolved one) fell off the gate's right side and never re-inferred.
`merge_tuple_returns` did walk return statements to fill UNKNOWN
slots, but only from `val->node_type` and `AST_LITERAL` value-shape
heuristics, not from `AST_IDENTIFIER` lookups, which is where
destructured locals' types live.

**Fix.** Extend the re-inference gate to also fire when the current
type is `TUPLE` with at least one `TYPE_UNKNOWN` slot. The companion
sync from `child->node_type` to `func_sym->type` inside the inference
loop also needed an extension: previously synced only when func_sym
was UNKNOWN/VOID; now also syncs when it's a TUPLE and the new type
is *strictly* more specific (fewer UNKNOWN slots). The strictness is
load-bearing: without it, syncing TUPLE(s, UNKNOWN) → TUPLE(s,
UNKNOWN) loops forever and exhausts `MAX_INFERENCE_ITERATIONS`.

## Bug 4, nested return doesn't pre-resolve outer-scope locals

**Symptom.** A multi-value `return a, b` inside an `if`/`while`/`for`
body can't see locals defined by destructure in the surrounding block.
Specifically, in:

```aether
hash_file(p: string) {
    bytes, length, rerr = fs.read_binary(p)
    if string.length(rerr) > 0 {
        return "", rerr                              // <-- right here
    }
    return cryptography.sha256_hex(bytes, length)
}
```

`rerr`'s `AST_IDENTIFIER` inside the `return "", rerr` has no
`node_type` set (no symbol-table entry survives across iterations
after the bug-1 unwind), and `infer_return_type_impl` for the nested
return falls back to `lookup_symbol`, which doesn't have it, so the
slot is UNKNOWN.

**Cause.** The `AST_BLOCK` case of `infer_return_type_impl` had a
pre-resolve pass for direct single-value `return v` returns,
calling `resolve_local_var_type(v, body, i, symbols)` to pull the
type from a preceding sibling. But:

- The pre-resolve only handled single-value returns, not multi-value.
- It only ran for direct children of the block, not returns nested
  inside an `if`/`while`/`for` body.

**Fix.** Add a new `preresolve_return_idents_in(node, outer_block,
outer_index, symbols)` walker. Called at the top of every
`AST_BLOCK` case, it walks the subtree looking for `AST_RETURN_STATEMENT`s
with multi-value children. For each untyped `AST_IDENTIFIER` slot, it
calls `resolve_local_var_type` from the *outer* block (where the
destructure that introduced the local lives) and stamps the result
onto `slot->node_type`. The recursive `infer_return_type_impl` then
sees a fully-typed tuple instead of one with UNKNOWN slots.

The walker also recognizes `AST_TUPLE_DESTRUCTURE` siblings in
`resolve_local_var_type`, projecting the matched lvalue's slot type
from either the rhs's resolved tuple type or, as a fallback, the
function symbol's declared tuple return type. Without this branch, a
multi-value destructure of `some_call()` couldn't be the source of a
local's type during return-type inference.

## Putting it together

The four bugs have a shared shape: the inference pipeline runs in
phases (constraint collection, propagation, return-type inference,
constraint solving) and converges by iteration. Any phase that writes
an UNKNOWN slot or a VOID type must be re-runnable when more
information becomes available; any phase that *needs* type info must
look it up through every avenue available (AST node_type, symbol
table, sibling-aware `resolve_local_var_type`, function symbol's
declared type) before defaulting to UNKNOWN.

When in doubt, the litmus test for whether the inference pipeline is
self-consistent is to compile the body of a function whose return
mixes a destructured local with a literal, inside a nested if-body.
If codegen prints `unresolved type in codegen, defaulting to int`, one
of the four pieces above is misbehaving.

## Working tests

| Test | What it proves |
|---|---|
| `tests/integration/tuple_destructure_cross_module/` | Bug 1: two modules with same-named locals don't collide. |
| `tests/integration/multi_return_destructure_chain/` | Bugs 2–4 in combination: empty-string literal in tuple slot, nested return in if-body, destructured local across the boundary. |

Run individually:

```sh
tests/integration/multi_return_destructure_chain/test_multi_return_destructure_chain.sh
```

Or via `make test-ae` for the full suite.

## What's out of scope

1. **Bidirectional type flow.** The current pipeline is bottom-up
   (returns drive function types, function types drive call-site
   types, call-site types drive caller's local types). It does not
   propagate caller's expectation back into a callee's return, a
   function whose body returns `(0, 0)` will still infer `(int, int)`
   even when every caller destructures it as `(string, string)`. The
   destructure-aware fallback at call sites prevents this from
   producing wrong code, but at-the-source reasoning would let the
   warning go away.

2. **Same-name lvalue in destructure and outer scope.**
   `mod_name = some_string()` followed by `mod_name, _ = some_call()`
   in the same block "reassigns" `mod_name` semantically. Type
   inference doesn't track reassignment, the second binding wins for
   subsequent reads but the first's type is what `resolve_local_var_type`
   returns when walking from a later return. In practice this hasn't
   bitten anyone yet because reassignment-with-different-type is rare
   in idiomatic Aether; flagged here so future cases get a consistent
   resolution rather than per-call hacks.

3. **Tuple slots resolved by union.** A function with two returns
   `(string, "literal")` and `(string, some_call())` whose call is a
   tuple-typed function, the slots are not unified. Whichever return
   is encountered first wins, and `merge_tuple_returns` fills only
   UNKNOWN slots. This is fine for the wrapper-style functions that
   dominate the stdlib; if it bites someone with a more complex
   shape, the fix is to widen `merge_tuple_returns` to take a
   "compatible-supertype" union rather than an UNKNOWN-only fill.
