# Unifying `T?` and `(value, err)` — costed design

> **Status: IMPLEMENTED.** The whole arc has shipped:
> - **P0** — `or {}` value-block miscompile fix (v0.398.0).
> - **P1** — migrate the 14 two-slot fallible signatures to `T!`.
> - **P1.5** — reject the ABI-incompatible tuple-payload `(A, B)!`, pinning
>   `T!` as single-payload; the 4 three-slot functions stay raw tuples by
>   design.
> - **P2** — a dropped `T!` result is a compile error.
> - **P3** — declared `fault`s: interned string-valued identities with
>   namespace-qualified cross-module identity via `==`.
> - **P1.3** — `??` accepts a fallible `T!` left side, one default operator
>   across both axes.
>
> Plus two `or {}` heap-leak fixes surfaced while implementing: the discarded
> error slot and the yielded value slot.
>
> Costed against the compiler as of v0.397.0; every claim below was verified
> against the tree (`file:line`) at design time, not recalled. Origin:
> `docs/cross-references/c3.md` §4 — the survey's "one large win": C3 proves
> one mechanism can serve both the "absent" and the "failed" axes. This
> document scopes the Aether version — and concludes the Aether version is
> **not C3's design**, for reasons the tree itself supplies.

## 0. Summary, and what the reconnaissance changed

The survey proposed C3's literal shape: make `T?` carry a `fault`, so `none`
and "failed" share one channel. Scoping that against the tree produced four
findings that reshape the design:

1. **Both mechanisms are lopsided in opposite directions.** `(value, err)`
   carries *all* real traffic: 279 error-return sites across `std/`, 119
   distinct error strings. `T?` carries *none*: zero optional returns, zero
   `??`, zero `?.` anywhere in `std/` — the entire optional surface (including
   flow narrowing) is exercised only by tests. So "unification" is not
   merging two live systems; it is deciding which of two half-used systems
   becomes the fallible spine.
2. **`T!` already exists and is ABI-identical to the convention.** `T!` IS a `(T, string)` tuple with one flag set (`Type.is_result`,
   `ast.h:414`; `create_result_type`, `ast.c:91-95`). The flag gates exactly
   three behaviours: bare `return v` auto-wraps to `(v, "")`
   (`codegen_stmt.c:3344-3353`), `expr!` propagates instead of panicking
   (`codegen_expr.c:2175-2214`), and `or` typing. **`std/` does not use `T!`
   anywhere** — the stdlib writes raw tuples. Migrating a signature to `T!`
   moves zero bytes at the ABI.
3. **The feared costs are empirically absent.** No error message in the tree
   is dynamic (all 279 sites use string literals; the `string_concat(x, "")`
   calls nearby are owning-copies of the *value* slot). No caller anywhere
   compares error *content* — only `err != ""` presence tests. And errors
   do not cross the FFI at all: `--emit=lib` refuses to export
   tuple-returning functions (`codegen.c:2229-2240`). The three
   scariest §4 costs — payload loss, comparison migration, ABI break — have
   nothing to break *today*. That is the window argument.
4. **A live miscompile sits in the foundations.** The value-producing block
   form of `or` — `x = f() or { -1 }` — silently discards the handler's
   value and yields an **uninitialized C local** (reproduced: `b=1396619984`).
   See §2. Fix-first, independent of any design decision.

**The recommendation in one paragraph (§6):** keep Aether's documented
two-axis split — `T?` stays "present or absent" — and make **`T!` the single
fallible mechanism**, in three small phases: (1) fix `or {}` and migrate
`std/` signatures from raw `(T, string)` tuples to `T!` (ABI-invariant,
mechanical); (2) enforce consumption on `T!` results — dropping one is a
compile error, which is `@nodiscard`'s best half arriving through the
type system, **opt-in per signature by construction** because `is_result`
is the bit; (3) give errors *identity* with declared, interned faults that
are still `const char*` — so every existing `err != ""` check, the whole
ABI, `defer catch`'s guard, and dynamic message strings all keep working,
while `err == fs.NotFound` becomes a pointer compare. C3's benefits —
one-char propagation, can't-forget-to-check, cheap identity — without C3's
payload loss and without fighting the axis split Aether documents as
deliberate. Estimated ~1,000–1,500 lines across three PRs (§8), each
independently shippable.

## 1. What Aether already owns (the §5.5 inventory)

Verified against the tree; this is the load-bearing section.

### 1.1 The `(value, err)` side

| Piece | Where | Notes |
| --- | --- | --- |
| Tuple lowering `{T0 _0; ...; const char* _N}` by-value | `ensure_tuple_typedef`, `codegen.c:3237-3259` | The error is always the **last** slot |
| The presence convention `e && e[0]` | five sites: `codegen.c:1156` (conditional defers), `codegen_expr.c:2101` (`or`), `:2194` (`expr!` propagate), `:2217` (`expr!` panic), `codegen_stmt.c:5903-5910` (multi-value return guard) | The single convention every consumer shares |
| `T!` = `(T, string)` + `is_result` | `ast.h:414`, `ast.c:91-95`, parsed `parser.c:283-285` | Auto-wrap, propagation, `or` typing — nothing else |
| `expr!` propagation (drains defers) | `codegen_expr.c:2122-2223` | Statically-known error exit (`DEFER_EXIT_ERROR`) |
| `or <expr>` default | `parser.c:1625-1653`, `codegen_expr.c:2078-2119` | Works; block form broken (§2) |
| `defer try` / `defer catch` | Fires on tuple **shape**, not `is_result` (`function_can_fail`, `typechecker.c:540-545`) | Already treats tuples and `T!` uniformly |
| Error creation in `std/` | 279 `return …, "literal"` sites; 119 distinct strings; **zero dynamic** | The "payload" is unexercised |
| Error comparison in the tree | **zero content comparisons**; presence-only | Identity is currently unexpressible, and nobody works around it |
| FFI | tuple-returning functions **not exported** (`codegen.c:2229-2240`) | No external ABI to preserve |

### 1.2 The `T?` side

| Piece | Where | Notes |
| --- | --- | --- |
| Lowering `typedef struct { int has; T val; } ae_opt_<T>` by-value | `ensure_optional_typedef`, `codegen.c:3261-3283` | Per-inner-type synthesis; different shape from tuples |
| `none` pinning (annotated decls, `==` operands, match arms; bare `let m = none` is an error) | `typechecker.c:4916-4949, 6667-6690, 5563-5565` | |
| Implicit wrap `T -> T?`, **no implicit unwrap** by design | `typechecker.c:1128-1140` | |
| `!` force-unwrap — **one parse node with `expr!`**, operand-type dispatch | `parser.c:2116-2130`; optional path `codegen_expr.c:2143-2151`, tuple path `:2152-2222` | The shared spelling already exists |
| `??` default (optional-only; rejects `T!` with a clean message) | `codegen_expr.c:2239-2253` | Vocabulary split with `or` is arbitrary today |
| `?.` chain read + documented no-op write | `codegen_expr.c:2255-2275`, `codegen_stmt.c:3401-3427` | |
| `match m { none / some(v) }` | `codegen_stmt.c:5306-5366` | |
| **Flow-sensitive narrowing**: `if x != none { … }` reads become `x.val`, zero runtime cost | `typechecker.c:4498-4582, 5121-5145`; `codegen_expr.c:2368-2377` | C3's §4.3 narrowing **already exists** on this side |
| Usage in `std/` | **zero** | Polished shelf-ware |

### 1.3 The compilation model (this decides the fault mechanism)

- **Ordinary builds are ONE translation unit.** `module_merge_into_program`
  (`aether_module.c:1934`) clones every imported module into the entry
  program; `aetherc` emits one `.c`. Within it, "fault identity = address of
  a global" is trivially sound — no linker coordination, *simpler* than C3's
  scheme (C3 needed linker-symbol uniqueness across separately compiled
  objects; Aether mostly doesn't).
- **But `binary_import` is real separate compilation**
  (`tests/integration/binary_import/`): a precompiled `--emit=lib` `.so`
  consumed via `import gizmo` through its `aether_lib_meta` catalog. A fault
  descriptor compiled into the `.so` and the "same" fault in the app are
  different globals with different addresses. Any identity design must
  survive that boundary. §6 phase 3 does: **pointer compare with content
  fallback** (interned-string comparison), correct everywhere, fast in the
  common one-TU case.

## 2. Bugs found while scoping (the recurring pattern)

Like `defer catch` (which surfaced the `expr!` defer leak, fixed first in
its own PR), scoping §4 found live defects in the foundations:

1. **`or { … }` value-block miscompile — ✅ FIXED (v0.398.0).**
   `x = f() or { -1 }` compiled and, on the error path, yielded an
   **uninitialized local**: the block's last expression was emitted as a
   discarded statement and the result slot never assigned
   (`codegen_expr.c:2103-2117` as surveyed; block handlers were designed as
   must-exit and the value-yielding form was neither implemented nor
   rejected). The fix implements last-expression-as-value (what §6's
   ergonomics assume) and REJECTS handler blocks that neither yield a
   value of the right type nor exit. Guarded by
   `tests/regression/test_or_block_value.ae` (verified failing on the
   unfixed compiler) and `tests/integration/or_block_reject/`.
2. **Noted, lower priority:** `T??` parses but is unguarded and fragile;
   optionals in tuples / struct-literal fields / actor messages are
   unexercised. None block §6; recorded so they're not rediscovered.

   (The former note here — that `==` on `string?` compares pointers, not
   content — is resolved: §366 and `language-reference.md` §Optionals both
   specify content comparison, and it is verified: two distinct allocations of
   the same text, typed `string?`, compare equal.)

## 3. What C3's design maps to here — and what doesn't

C3's mechanism (survey §4.2): an Optional is a (value, fault) pair in two
storage slots; `fault` is a pointer-width integer, `0` = no error, otherwise
the address of a link-time symbol whose content is the fault's name string.
The ABI rewrite `T? f(args)` → `fault f(T* out, args)` is the whole trick.

What transfers cleanly to Aether:
- **Identity as an interned pointer whose content is the name.** Printable
  for free, comparable for free, composable across modules for free —
  *especially* free in a one-TU world (§1.3).
- **One-character propagation** — already shipped (`expr!` in a `T!`
  function).
- **Mandatory consumption** — C3's "you cannot forget to check" is its
  deepest safety property and Aether's biggest gap: today `fs.write(path,
  data)` with the error ignored compiles silently.

What does not transfer:
- **Merging "absent" with "failed".** Aether's docs are explicit
  (`language-reference.md` §Optionals: *"the type for 'maybe a value',
  distinct from the (value, err) result convention, which is for fallible
  operations"*; same position in `LLM.md`). C3 conflates them — `none` and
  `fault==0` are the same state — and C3 then can't express "this lookup
  succeeded and found nothing" distinctly from "this lookup failed".
  Aether's split is *better*, and the tree contains a working narrowing
  implementation on the `T?` side that assumes it. Fighting this would be
  copying a borrowed feature's full surface instead of the missing part —
  the exact mistake §5.5 of the survey warns against.
- **The ABI rewrite.** C3 needed it because its Optionals are front-end
  fictions over two slots. Aether's tuples already return by value, nothing
  external consumes them (§1.1), and the rewrite would buy a register-return
  micro-win at the cost of touching every call site's emission. Cost it,
  decline it (§7).
- **Faults as the *only* error payload.** C3's sharpest weakness. Aether's
  error slot is a string that *could* carry data (even though today none
  does) — §6 keeps that ability instead of trading it away.

## 4. The design question stated plainly

Two coherent end-states:

- **(A) C3-faithful:** `T?` grows a fault slot; `none` = no-fault;
  `(value, err)` and `T!` retire. One type serves both axes.
- **(B) Aether-shaped:** `T!` becomes the one fallible mechanism (typed,
  enforced, identity-bearing); `(value, err)` raw tuples migrate to it
  signature-by-signature; `T?` remains pure presence, untouched.

## 5. Option A, costed and declined

What it requires: re-lower `ae_opt_<T>` to carry a fault (representation
change for every optional), decide `none`-vs-fault semantics for `??`/`?.`/
`match`/narrowing (each currently presence-based), migrate all 279
`(value, err)` sites AND the operators' semantics simultaneously, and
re-document the axis split as a mistake. It reaches C3 parity, including
C3's inability to distinguish "empty" from "failed" — a regression for any
lookup-shaped API (`map.get`, "first element", search) that legitimately
wants `T?` for absence *and* an error channel for I/O failure.

Blast radius: every optional test, every fallible signature, both operator
families, the narrowing pass — in one coordinated change. Estimated 3–5×
Option B's cost, for a semantic model the language's own docs argue against.
**Declined.** (If a future need arises for "fallible optional", `T?!` —
an optional inside a result — composes from Option B's parts for free.)

## 6. Option B (recommended): `T!` as the fallible spine

Three phases, each independently shippable, each leaving the tree green.

### Phase 1 — repair and converge (no new semantics)

1. **Fix the `or {}` value-block bug** (§2.1 — its own PR, first).
   *Shipped in v0.398.0.*
2. **Migrate `std/` signatures** from `-> (T, string)` to `-> T!`.
   ABI-invariant (§0.2): callers' `v, e = f()` destructuring, `e != ""`
   tests, `defer catch`, everything continues byte-identically. ~17
   top-level fallible signatures by conservative grep (regex over
   single-line `-> (…string)` forms; the long tail is wrappers), 279
   return sites — mechanical, reviewable module-by-module (fs, io, os,
   json, regex first).

   **Boundary found during migration: `T!` is
   single-payload only.** `create_result_type(inner)` wraps its argument,
   so a tuple payload `(A, B)!` lowers to the *nested* tuple
   `((A, B), string)` — C layout `{ _tuple_A_B _0; const char* _1; }` —
   which is **not** ABI-compatible with the *flat* `(A, B, string)` tuple
   (`{ A _0; B _1; const char* _2; }`) the multi-value stdlib functions
   return; a 3-way `a, b, e = f()` destructure sees only 2 values and
   fails downstream. So the 14 two-slot `(T, string)` functions migrated
   to `T!`; the 4 three-slot functions (`json.parse_strict`,
   `json.object_entry`, `regex.find`, `regex.find_lit`) **stay raw
   tuples** — Phase 2 enforcement keys on `is_result`, so they are simply
   not-yet-enforced (gradualism is structural). `(A, B)!` is now a parse
   error with guidance pointing at the raw-tuple form (migration piece 1.5).
3. **Vocabulary convergence, minimal:** make `??` accept a `T!` left side
   (same lowering as bare-expression `or`; today it cleanly rejects). One
   default operator across both worlds; `or` remains for block handlers.
   Optional — costs ~40 lines, drop if contentious.

   *Related bug found: the `or {}` lowering does not free a
   heap error slot it discards on the error path — `x = json.parse(s) or
   {…}` leaks the error string when the parse fails. Pre-existing, orthogonal
   to migration; tracked separately.*

### Phase 2 — enforcement (the safety payoff)

**Discarding a `T!` result is a compile error.** A call to a `T!`-returning
function used as a bare expression statement must be consumed: destructured,
`!`-propagated, `or`-handled, or explicitly discarded (`_, _ = f()`).

- This is the survey's `@nodiscard` (§7) arriving through the type system,
  scoped to exactly the resource that matters most — the error channel —
  which was always its best half. (§7's remaining half, memory, is already
  RAII's job here.)
- **Gradualism is structural, not scheduled:** enforcement keys on
  `is_result`, so raw `(T, string)` tuples are untouched forever, and each
  signature opts in the moment its module migrates in Phase 1. No flag day
  exists to schedule. This is the same shape that made contract folding's
  behavioural change safe (~zero blast radius at ship time).
- Implementation: a typechecker check on expression-statements whose type
  `is_result` — same neighbourhood as the existing statement walks. The
  hard part is enumerating the legitimate consumers; the `e && e[0]` site
  list (§1.1) is the checklist.

### Phase 3 — identity: declared faults over `const char*`

```aether
// std/fs/module.ae
fault NotFound, PermissionDenied, IsDirectory

read(path: string) -> string! {
    ...
    if missing { return "", fs.NotFound }        // a fault IS a string value
    if denied  { return "", fs.PermissionDenied }
}

// caller
data, err = fs.read(p)
if err == fs.NotFound { return default_config() }   // identity compare — NEW
if err != "" { return "", err }                      // presence — unchanged
```

Design pinned by the compilation model (§1.3):

- **A `fault` declaration mints an interned global whose content is its
  qualified name**: `static const char aether_fault_fs_NotFound[] =
  "fs.NotFound";` — and the fault *value* is that pointer. The error slot's
  type does not change: it is still a string. Consequences, each verified
  against an inventory item:
  - Every `e && e[0]` site (§1.1) works untouched — a fault is non-empty.
  - Printing works untouched — the value *is* its name (C3's trick).
  - Dynamic error strings remain legal — the payload door stays open,
    unlike C3.
  - The ABI does not move — `const char*` in the same tuple slot.
- **`err == fs.NotFound` lowers to pointer-compare-then-content-compare**
  (`err == F || (err && strcmp(err, F) == 0)`). In the ordinary one-TU
  world the pointer path always hits. Across a `binary_import` boundary —
  where the `.so`'s descriptor is a different global — the `strcmp`
  fallback keeps identity correct. Classic interning; no registry, no
  ordinals, no catalog changes required (though `aether_lib_meta` *may*
  later advertise fault tables for tooling).
- **`fault` is a contextual keyword** at top level (the `enum`/`bitstruct`
  interception pattern, `parse_top_level_decl`); member access `fs.NotFound`
  rides the existing namespaced-constant rewrite.
- **What this deliberately does not do:** no exhaustiveness over fault sets
  (C3 doesn't have it either — its `@return?` is a partially-checked lint,
  survey §4.5), no fault payloads-with-data in v1 (a `fault_is(err, F)`
  prefix-match helper over `"fs.NotFound: /etc/foo"`-style strings is a
  clean later extension), no changes to `T?`.

### What Option B never touches

`T?` semantics, the `ae_opt` representation, narrowing, `match`, `?.` — the
presence axis stays exactly as documented. The unification is of the
*fallible* side with itself: one typed mechanism where today there is a
typed mechanism nobody uses plus an untyped convention everybody uses.

## 7. Considered and declined within Option B

- **C3's ABI rewrite** (`fault f(T* out, args)`): no external consumer
  exists to benefit (§1.1 FFI), by-value tuple return is already the ABI,
  and the change would touch every call-emission path for a micro-win.
  Revisit only if `--emit=lib` ever wants to export fallible functions —
  at which point the fault-returning shape is the *natural* export ABI,
  and Phase 3's design is forward-compatible with it.
- **Making `!` mandatory-consumption's discard spelling** (`f()!` to
  ignore): `!` already means propagate-or-panic; overloading it to "ignore"
  would invert its meaning. Explicit `_, _ =` is the discard.
- **Narrowing for `T!`** (`if e == "" { use v unchecked }`): real value,
  real cost (the narrowing machinery generalized). Not needed for phases 1–3;
  natural fourth phase if wanted.

## 8. Costing

| Phase | Piece | Est. lines | Risk |
| --- | --- | --- | --- |
| 0 | `or {}` value-block fix + tests (own PR) | 60–120 | low — one emission branch |
| 1 | `std/` signature migration to `T!` (17 sigs, 279 sites reviewed) | 300–600 churn | low, mechanical; the review burden IS the cost |
| 1 | `??` accepts `T!` (optional) | ~40 | low |
| 2 | discard-check in typechecker + diagnostics | 100–150 | low — expression-statement walk |
| 2 | tests: reject suite + silence suite (consumption forms) | 150–250 | — |
| 3 | `fault` declarations: parser + interned-global emission | 120–180 | low — enum/bitstruct pattern |
| 3 | `==`-on-fault lowering (ptr-then-strcmp) + typecheck | 80–120 | low |
| 3 | tests incl. a **binary_import cross-boundary identity test** | 200–300 | the one genuinely novel test |
| — | docs (language-reference, LLM.md, CHANGELOG ×3) | 200–300 | — |
| | **Total, phases 0–3** | **~1,200–2,000** | three-to-four PRs |

Calibration: `bitstruct` ~1,050 staged; `defer catch` ~620; contract folding
~1,234. Each phase here is at or below that band, and no phase depends on a
later one landing.

## 9. Test plan (what "done" means per phase)

- **P0:** `or { -1 }` yields −1 on the error path (valgrind-clean); early
  return form unchanged; regression pinned against the uninitialized read.
- **P1:** full suite green with migrated signatures — the suite itself is
  the test, since callers are supposed to be unaffected; `defer catch`
  suites re-run unchanged.
- **P2:** reject: bare `fs.write(p, d)` statement errors; silence: all
  consumer forms accepted (`v,e =`, `!`, `or`, `_, _ =`); raw-tuple
  functions never flagged.
- **P3:** identity: `err == fs.NotFound` true on the fault, false on a
  same-text dynamic string? — **No**: content-equal strings compare equal
  by design (that is what makes it interning, not object identity). True on
  the fault, true on a strcmp-equal string, false otherwise; pointer path
  verified by inspecting emitted C; **cross-`.so` identity via
  binary_import**; presence tests and printing unchanged on fault values.

## 10. Recommendation

Ship Phase 0 (the `or {}` fix) immediately — it is a miscompile regardless
of any design decision. Then phases 1–3 in order, one PR each, pausing
after each for the same review this document got. The window argument from
contract folding applies at larger scale: `std/` gains fallible signatures
every release, every one written against the stringly convention, and
`T!`'s opt-in enforcement bit means the migration can never be cheaper
than now. The survey called §4 "the single biggest ergonomic + safety win
available"; the tree says the win is real but the shape is not C3's — it
is `T!`, which Aether already built and then never used.
