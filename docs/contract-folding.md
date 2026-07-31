# Compile-time contract folding, costed design

> **Status: ✅ IMPLEMENTED**, shipped as designed, both tiers,
> error-from-day-one, with the evaluator unification of §6. One design
> revision made during implementation and worth recording: the doc specified
> "value-precise" short-circuit folding, but the pre-existing contracts doc
> *promises* that `x || true` keeps its runtime check (the unknown operand
> may carry a side-effectful call). The shipped evaluator is therefore
> **asymmetric**: FALSE-dominance is symmetric (a FALSE verdict never skips
> runtime evaluation, it becomes a build error or an emitted check), while
> TRUE-dominance is left-to-right only, mirroring runtime short-circuit
> order, so elision never skips an effect the runtime would have run.
>
> Costed against the compiler as of v0.396.0; every claim below is verified
> against the tree (`file:line`), not recalled. Origin:
> `docs/cross-references/c3.md` §10.2, C3's `@require` doubles as a
> compile-time constraint, giving concepts/trait-bound-like checking without
> a macro system. Line references below describe the tree as surveyed,
> pre-implementation.

## 0. Summary and the surprise

The survey (§10.2) proposed: *"make Aether's contracts fold at compile time
when their operands are known, erroring rather than trapping."* Costing it
against the tree found that **half of it already shipped**, unrecorded in the
survey:

- `try_fold_predicate` (`compiler/codegen/codegen_stmt.c:3086`) already
  evaluates contract predicates over literals, arithmetic, comparisons,
  logical ops, unary `!`/`-`, and **elides constant-true checks** from the
  emitted C (`emit_contract_check`, `codegen_stmt.c:3140`), leaving a
  `/* precondition elided (always-true) */` comment. Guarded by
  `tests/regression/test_contract_const_fold.ae`.
- Constant-**false** was considered and **deliberately left as a runtime
  panic**, the comment at `codegen_stmt.c:3129-3132` says: *"the runtime trip
  is observable to the test suite without aetherc having to refuse the
  build."*

So this design is **not** "add contract folding." It is two smaller things:

1. **Tier 1, definition-site constant-false becomes a compile error** (a
   reversal of the documented decision above, argued in §3).
2. **Tier 2, call-site folding**: substitute the actual arguments into the
   predicate at each call site and error when it is decidably false. This is
   the concepts-like payoff and the genuinely new machinery.

```aether
divide(a: int, b: int where b != 0) -> int { return a / b }

x = divide(10, 0)     // Tier 2: compile error, today a runtime panic
```

Total estimated cost: **~700–950 lines all-in** (compiler + tests + docs),
one PR. Smaller than `bitstruct` (~1,050 staged) and comparable to
`defer catch` (~620). Details in §7.

## 1. What Aether already owns (the §5.5 check)

Per the method note in `docs/cross-references/c3.md` §13: before porting,
enumerate what the host already has, and cut the feature down to the missing
part. The inventory:

| Piece | Where | State |
| --- | --- | --- |
| Contract syntax: `requires` / `ensures` / `param: T where cond` | parsed to `AST_REQUIRES_CLAUSE` / `AST_ENSURES_CLAUSE` on the function (`parser.c:4655` for `where`) | ✅ shipped |
| Runtime checks: `if (!(pred)) aether_panic("… violation: <text> in <fn>")` | `emit_contract_check`, `codegen_stmt.c:3126+`; suppressed by `--no-contracts` | ✅ shipped |
| Constant-**true** elision | `try_fold_predicate` + `emit_contract_check` (`codegen_stmt.c:3086,3140`) | ✅ shipped |
| Predicate-to-text renderer (for messages) | `sprint_expr_text`, static in `codegen_stmt.c` | ✅ shipped, but codegen-private |
| Const-ness *classifier* (not evaluator) | `is_const_expression`, `typechecker.c:884` | ✅ shipped |
| Compile-time bool evaluator (narrow: `target.os/arch`, string/bool/num literals) | `when_eval_condition`, `optimizer.c:407` | ✅ shipped, `when`-specific |
| Call-argument boundary walk (where the distinct-type and bitstruct checks live) | `typechecker.c:7622` area | ✅ shipped, **this is Tier 2's hook** |
| Enum member values known at compile time | `resolve_enum_types` rewrites `E.M` to a constant identifier before typecheck (`typechecker.c` enum machinery) | ✅ shipped |
| **Const evaluator with an environment** (param → argument, const idents) |, | ❌ **the missing piece** |
| **Any compile-time contract *diagnostic*** (vs elision) |, | ❌ missing |

The genuinely-missing part is one evaluator and its wiring. Everything else is
reuse.

## 2. Two prior decisions this design must respect (or overturn explicitly)

**(a) The whitelist-only const layer is a security boundary, not a
limitation.** The comment block at `typechecker.c:826-851` is explicit: a
general compile-time evaluator could synthesize `std.fs` / `std.net` calls
past the `--emit=lib` capability gate, so const evaluation is whitelist-only
*by design*. The contract evaluator therefore **must not evaluate calls**
not even the pure-conversion whitelist. Predicates fold over literals,
constants, enum members, arithmetic, comparisons and logical ops; anything
else is UNKNOWN and stays a runtime check. (This also matches what
`try_fold_predicate` already does.)

**(b) `when` arms are pruned before typecheck** (`optimizer.c:318-319`:
*"pruned here, BEFORE typecheck runs"*). This is load-bearing for Tier 2: a
`divide(x, 0)` inside a `when target.os == "windows"` arm cannot
false-positive a Linux build, because the typechecker never sees it. Without
this ordering, call-site erroring would be unshippable.

## 3. Tier 1, definition-site constant-false → compile error

**Behaviour.** A `requires` / `where` / `ensures` predicate that folds to
constant false is a compile error at the function definition:

```
error: `requires` predicate is always false — this function can never be
called legally: N > 10 (N is the constant 5) in resize
```

**Why reverse the documented decision?** The `codegen_stmt.c:3129` rationale
("observable to the test suite without refusing the build") made sense when
the fold's only job was *elision*, a perf feature shouldn't change what
compiles. But a constant-false contract at the definition is a bug with
probability ~1: nobody writes `requires false` on purpose, and the realistic
case is a **refactor that stales a constant** (`requires cap > MIN` where a
`const MIN` edit makes it unsatisfiable). Deferring that to the first runtime
call is strictly worse. C3 makes the same call (`sema_stmts.c:415-444`:
const-false contract → `sema_error_at`, const-true → emit nothing).

**Where it lives.** The typechecker, not codegen, errors must fire before
emission, and `type_error` lives there. The walk is trivial: for each
function's `AST_REQUIRES_CLAUSE` / `AST_ENSURES_CLAUSE`, run the evaluator
with an **empty environment**; error on decided-false. Codegen's true-elision
is untouched.

**Cost:** ~40–60 lines + reject tests. No new machinery (uses Tier 2's
evaluator with an empty env; if Tier 1 shipped alone it could call a lifted
`try_fold_predicate`).

## 4. Tier 2, call-site folding (the actual feature)

### 4.1 Behaviour

At every call to a function with contract clauses, build the environment
`{param name → argument expression}` and evaluate each `requires`/`where`
predicate under it:

| Evaluation result | Action |
| --- | --- |
| **false** | compile error at the call site, naming the predicate, the function, and the offending constant argument |
| **true** | nothing (the runtime check still exists at function entry; see §4.4) |
| **unknown** (any operand not compile-time-known) | nothing, runtime check as today |

```aether
divide(a: int, b: int where b != 0) -> int { … }

divide(10, 0)        // error: precondition b != 0 of divide is violated:
                     //        argument `b` is the constant 0
divide(10, n)        // unknown (n is runtime) → runtime check, as today
divide(10, 5)        // true → fine (runtime check folds per §4.4 note)
```

Multi-parameter predicates work with the same env, `requires a < b` folds
when *both* arguments are constants, else UNKNOWN:

```aether
clamp(lo: int, hi: int) -> int
    requires lo <= hi { … }

clamp(9, 3)          // error at the call site
clamp(9, n)          // runtime check
```

### 4.2 The evaluator

A new tri-state evaluator in `compiler/analysis/` (beside the typechecker):

```
ConstEvalResult contract_eval(ASTNode* pred, ContractEnv* env, ConstVal* out)
  → CONST_TRUE / CONST_FALSE / CONST_UNKNOWN
```

Domain rules, each load-bearing:

- **Integer domain is `int64_t`, not `double`.** `try_fold_predicate` is
  double-based, which is fine for its literal-only elision job but wrong for
  a *diagnostic*: `requires x == 9007199254740993` folded through a double
  compares equal to `…92` (2⁵³ precision loss) and would error on correct
  code. The new evaluator carries a tagged `{int64 | double | bool}` value
  and only falls to double when a float literal enters. (This also resolves a
  latent discrepancy: `test_contract_const_fold.ae`'s header claims `%` is
  folded, but `try_fold_predicate` has no `%` case, unsurprising, since
  `%` has no clean double semantics. Int64 domain supports `%` exactly.)
- **Identifier resolution, in order:** (1) the call-site env (param →
  argument, where the argument must itself evaluate to a constant); (2)
  module `const` declarations whose initializer `is_const_expression`
  accepts (evaluated recursively, depth-capped); (3) enum-member constants
  (already rewritten to identifiers with known values by
  `resolve_enum_types`). Anything else → UNKNOWN.
- **No calls, no member access, no array indexing**, UNKNOWN, per §2(a).
- **Division/modulo by a folded zero → UNKNOWN**, never a fold-time trap
  (matches `try_fold_predicate`'s `/` guard). The *predicate* `b != 0` is
  what catches the bug; the evaluator must not crash evaluating `a / b`
  inside some other predicate.
- **Short-circuit semantics**: `false && X` is FALSE even when `X` is
  UNKNOWN; `true || X` likewise TRUE. This is what makes composite
  predicates useful rather than all-or-nothing.

### 4.3 The hook

`typechecker.c:7622`, the existing call-argument walk where the
distinct-type and bitstruct boundary checks live. It already iterates
params against argument slots. Additions: build the env while walking; after
the walk, fetch the callee's clauses (the callee's `ASTNode` is reachable the
same way the distinct check finds param types) and evaluate each. Diagnostics
need predicate text in the typechecker: `sprint_expr_text` is codegen-static,
so either move it to a shared TU or add a compact renderer, the repo
precedent for small deliberate duplication with a sync comment is
`is_const_expr_call` (`typechecker.c:858`: *"Kept byte-for-byte in sync
with…"*). Recommendation: **move it** (it has no codegen dependencies) rather
than duplicate an escaping-sensitive renderer.

### 4.4 What Tier 2 does *not* do

- **No per-call-site check elision.** The runtime check is emitted once at
  function entry, shared by all call sites; folding TRUE at one site can't
  remove it (another site may pass runtime values). Elision stays what it is
  today: definition-site, constant-true only. Call-site folding is purely a
  *diagnostic*. (Per-site specialisation is a perf feature someone could
  build later; it is out of scope and mostly worthless since gcc removes
  the entry check when it can prove it anyway.)
- **No `ensures` call-site folding.** `ensures` predicates reference
  `result`, which is unknowable at the call site without evaluating the
  body. Definition-site Tier 1 covers `ensures false`.
- **No path sensitivity.** `if false { divide(1, 0) }` errors. See §5.
- **No cross-function propagation** (a const flowing through a helper is
  UNKNOWN). That is dataflow analysis, a different project.

## 5. The behavioural change, stated honestly

Code that **compiles today and panics at runtime** (or never executes) will
**fail to build**:

```aether
if never_true() { r = divide(x, 0) }    // compiles today; rejected after Tier 2
```

Three mitigations, in decreasing strength:

1. `when`-arm pruning (§2b) removes the entire platform-conditional class
   the common legitimate reason for dead calls.
2. The tree today has **3 `where` clauses and 29 `requires`/`ensures`**
   across `std/`, `contrib/`, `examples/`, `tests/` (grep, 2026-07-15), so
   the blast radius of the flag-day is effectively zero *now*, which is the
   argument for doing this **before** contracts proliferate, not after.
3. A deliberate escape hatch already exists: route the argument through a
   runtime identifier (`z = 0; divide(x, z)`); the evaluator's env only
   binds constant arguments.

**Decision point, error vs warning.** Two defensible designs:

- **(A) Error from day one** (recommended). A decidably-false precondition at
  a reachable call site is a bug by construction; Aether's contracts panic
  loudly, at runtime for exactly this case, so the compile-time form is the
  same judgement made earlier. C3 errors. The 32-clause blast radius makes
  the flag-day free. Structured so the verdict is one switch: flipping to
  warning is a one-line change if the field disagrees.
- **(B) Warning for one release, then error.** Matches the "gradual
  contracts" branding, costs one release of latency, and risks the
  warning being ignored precisely where it matters.

The implementation is identical either way; only the final `type_error` vs
`type_warning` call differs. Recommend (A); the PR should state it in the
CHANGELOG under an `### Upgrade notes`-style caveat despite the tiny radius.

**`--no-contracts` interaction:** folding diagnostics **still fire** with
`--no-contracts`, they are semantic analysis, cost nothing at runtime, and
suppressing correctness findings because runtime *checks* were disabled would
conflate two different things. (C3 behaves the same way: `--safe=no` removes
the runtime asserts; const-false contract errors are sema and remain.)

## 6. Relationship to the two existing evaluators

After this ships the tree has three const evaluators with distinct jobs:

| Evaluator | Domain | Job | Divergence risk |
| --- | --- | --- | --- |
| `when_eval_condition` (optimizer) | bool/string + `target.*` | prune `when` arms pre-typecheck | none, different input language |
| `try_fold_predicate` (codegen) | double, literal-only | elide constant-true runtime checks | **benign but real**, see below |
| `contract_eval` (analysis, new) | int64/double/bool, env-carrying | compile-time diagnosis |, |

`try_fold_predicate` and `contract_eval` evaluate the *same* predicates at
definition sites. Divergence is benign in both directions (typechecker
errors → codegen never runs; typechecker misses → codegen still elides or
emits a runtime check), but sloppy. Option: have `emit_contract_check` call
the shared evaluator with an empty env and delete `try_fold_predicate`
(−60 lines, one source of truth). Recommended, as a mechanical final step
it also fixes the double-precision wobble in the elision path for free.

## 7. Costing

| Piece | Est. lines | Risk | Notes |
| --- | --- | --- | --- |
| `contract_eval` + env + int64/double value | 250–320 | **the risk concentrate**: precision, short-circuit, const-ident recursion (depth cap) | pure function; property-testable in isolation |
| Tier 2 hook in the call-arg walk | 80–120 | low, pattern established by the distinct/bitstruct checks at `typechecker.c:7622` | env built during the existing iteration |
| Tier 1 definition-site walk | 40–60 | low | empty-env reuse of the same evaluator |
| Move `sprint_expr_text` to shared TU + diagnostics | 60–100 | low | message format mirrors the runtime panic text, so the two read as the same violation |
| Unify `emit_contract_check` onto the shared evaluator, delete `try_fold_predicate` | −60 net | low | keeps `test_contract_const_fold.ae` green as the guard |
| Tests: positive folding, reject suite (`tests/integration/contract_fold_reject/`, ~6–8 cases modelled on `bitstruct_reject`), **silence suite** (runtime args, partial env, call operands, member access must NOT error), Makefile prune entry | 250–350 |, | the silence suite is the important one: false positives are this feature's only way to fail badly |
| Docs (`language-reference.md` contracts section, `LLM.md` bullet, CHANGELOG + upgrade note) | 100–150 |, | |
| **Total** | **~700–950** | | one PR; no new tokens, no parser changes, no ABI impact |

Prior calibration from this repo: `bitstruct` ~1,050 staged lines (parser +
types + codegen), `defer catch/try` ~620 (parser + typechecker + codegen).
This lands between them with **no parser work at all**, which is why the risk
sits almost entirely in one pure function.

## 8. Test plan (what "done" means)

1. **Reject:** `divide(10, 0)` with `where b != 0`; `clamp(9, 3)` with
   `requires lo <= hi`; const-through-`const N` staleness; `requires false`
   at definition; enum-member constant violating a range predicate.
2. **Silence (anti-false-positive):** runtime-variable args; one-of-two
   const args on a two-param predicate; predicate containing a call /
   member access / array index; `divide(1, 0)` sited inside a pruned `when`
   arm (must not error on the non-selected platform); int64 edge
   `x == 9007199254740993` with the exact constant (must fold TRUE, not
   FALSE, the double-domain trap).
3. **Elision regression:** `test_contract_const_fold.ae` stays green after
   the evaluator unification.
4. **Interaction:** `--no-contracts` still produces the fold errors;
   `HARDEN=1 make ci` clean.

## 9. Recommendation

Ship Tiers 1+2 together in one PR, error-from-day-one (§5A), with the
evaluator unification (§6) included. The window argument matters more than
the feature's size: at 32 contract clauses in the entire tree, the
compile-time-vs-runtime semantics of contracts can still be changed for
free. Every release that adds contract-using code makes the flag-day more
expensive. The C3 survey called this "a small change with real payoff"; the
tree says it is even smaller than that, half of it shipped with the contracts work, and
what remains is one carefully-tested pure function and its wiring.
