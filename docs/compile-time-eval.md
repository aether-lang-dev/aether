# Compile-time constant evaluation (phase-1)

Aether's `const` is *substitution-at-each-use*: the compiler inlines the
RHS expression at every reference rather than allocating storage. For a
literal (`const PI = 3.14`) that is exactly right. For an arbitrary call
(`const G = make_thing()`) it is silently wrong, every reference would
re-run the call, re-allocating / re-side-effecting. The typechecker
therefore rejects non-constant const initializers.

Phase-1 extends the set of initializers the compiler can
evaluate *at compile time*, folding them to a plain literal before
codegen so they inline correctly and run once, not per use. It is a
**hard-whitelisted folder, not an interpreter**.

## What folds

| Initializer | Folds to | Notes |
|---|---|---|
| `2 + 3 * 4` (arithmetic on numeric literals: `+ - * / %`) | `14` | Predates phase-1; in the correct width (int = 32-bit, float = double). Division/modulo by zero is *not* folded. |
| `string.from_int(<int const>)` | `"42"` | Folded in 32-bit width. |
| `string.from_long(<int const>)` | `"9999999999"` | Folded in 64-bit width. |
| `string.from_float(<num const>)` | `"3.14"` | `%g` formatting. |
| `string.concat(<str const>, <str const>)` | `"ab"` | Both operands must be string literals (or fold to them). |
| `const A: int[3] = [1, 2, 3]` / `const A[] = [...]` | `static const int A[] = {1, 2, 3}` | Const array literal. Every element must itself be a compile-time constant. |

Folds compose: `string.concat(string.from_int(1), "x")` folds to `"1x"`
because the optimizer recurses into arguments before folding the
enclosing call.

These also work inside another const: `const HALF = MAX / 2` where `MAX`
is itself a const.

## What does NOT fold (and why)

- **Any function call outside the whitelist**, e.g. `string.upper(...)`,
  `string.trim(...)`, a user function, an `extern`, `malloc`. Even though
  some of these are pure, they are rejected in a const initializer with:

  > `const initializer must be a compile-time constant expression, …`

  This is deliberate. A *general* compile-time evaluator would be able to
  synthesize `std.fs` / `std.net` calls and evaluate them at build time,
  which would run filesystem / network code **past the `--emit=lib`
  capability gate**, the gate that decides which side effects a compiled
  artifact is allowed to perform. By folding *only* an explicit,
  audited whitelist of pure value conversions, the build step performs no
  I/O and grants no capability it would not otherwise grant.

- **`string + string`**, the `+` operator is not defined for strings
  anywhere in Aether (it is rejected at typecheck with
  `'+' is not defined for strings`). Build a constant string with
  `string.concat(...)` instead. `const G = "a" + "b"` does **not** work
  by design; `const G = string.concat("a", "b")` does.

- **Struct literals**, **member access**, and **non-constant array
  elements**, rejected; each would either allocate fresh per use or is
  not a value the substitution model can inline.

- **Out-of-range / undefined folds**, a `string.from_int` argument that
  does not fit in 32 bits, or a division by zero, is **not** folded. The
  expression is left as-is rather than producing a silently-wrong
  literal. (For `from_int` the result would have been the runtime
  32-bit-truncated call, so leaving it untouched preserves existing
  behaviour exactly.)

## Where it lives

- **Whitelist gate (accept/reject):** `is_const_expression` /
  `is_const_expr_call` in `compiler/analysis/typechecker.c`. Runs during
  typecheck; admits exactly the whitelist, rejects everything else with a
  source-mapped diagnostic.
- **Folder (call → literal):** `fold_const_string_call` and the
  `AST_FUNCTION_CALL` / `AST_CONST_DECLARATION` arms of
  `optimize_constant_folding` in `compiler/codegen/optimizer.c`. Runs
  after typecheck, before codegen. The two lists are kept byte-for-byte
  in sync (`is_whitelisted_string_call` ↔ the typechecker whitelist).
- **Lowering:** a folded scalar const lowers to `#define NAME (value)`; a
  const array lowers to `static const T NAME[] = {…}`
  (`compiler/codegen/codegen_stmt.c` for locals,
  `compiler/codegen/codegen.c` for module-level).

## Phase-1 scope

Phase-1 is intentionally narrow: arithmetic on numeric literals, the four
whitelisted `string.*` conversions, and constant array literals. There is
no general const evaluator, no compile-time loop unrolling beyond the
existing closed-form loop passes, and no evaluation of user functions. Any
broadening of the whitelist must preserve the capability-gate invariant
above: only pure, I/O-free value conversions are ever eligible.

## Purity introspection: `__pure(fn)`

A separate whole-program analysis assigns every function a **purity** bit, and
the compile-time builtin `__pure(funcName)` folds to a `true`/`false` constant
reflecting it:

```aether,fragment
let safe = __pure(transform)           // bool constant, no runtime cost
if __pure(parse_config) { ... }        // branch on purity; folded to a literal
```

`__pure` folds during typecheck, so it is usable anywhere an expression is
(a `let` initializer, an `if` condition, a comparison). It is *not* a valid
`when` condition: `when` is resolved before typecheck (`resolve_when_statements`,
`compiler/codegen/optimizer.c`), so it never sees the folded bool and rejects
`when __pure(fn)` with "`when` condition is not a compile-time constant".
`when` conditions are limited to `target.os`/`target.arch` comparisons, bool
literals, and `&&`/`||`/`!` combinations of those.

A function is **pure** when, transitively, it:

- reaches no fs/net/os capability call (`file.*`/`dir.*`/`path.*`, `tcp.*`/
  `http.*`, `os.*` time/exec live under `os`), and
- mutates no caller-visible state, neither a parameter's pointee (`p.field =
  …`, `p[i] = …`) nor a module-level global.

The analysis is **conservative**: a function whose body it cannot see (an
`extern`, or an unresolved name) is treated as impure, and a raw `extern` call
inside a body is opaque and left unjudged (the same boundary the `--with=` gate
and effect tags have). This is the determinism axis that complements the
capability (access-control) axis, a function can be capability-empty yet
impure (reads a clock handed to it), or hold `fs` yet be pure for fixed input.
`__pure` resolves at compile time, so it gates optimizations and library
features with zero runtime cost.
