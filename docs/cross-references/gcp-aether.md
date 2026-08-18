# GoogleCloudPlatform/Aether vs. Aether, feature comparison

> **Status:** Design-exploration survey (May 2026). A record of what was considered from GoogleCloudPlatform/Aether (a separate, stalled project that shares this project's name) and what, if anything, was worth adopting, not a roadmap. Adopted items are noted inline; anything not yet shipped is a candidate, not a commitment.
>
> Source project: GoogleCloudPlatform/Aether, <https://github.com/GoogleCloudPlatform/Aether>.

This document is a compare-and-contrast against `GoogleCloudPlatform/Aether`
(hereafter **GCP-Aether** or just **GA**), a stalled "vibe-coded
demo" repo from Google that happens to share this project's name,
it appeared later than Aether. License Apache-2.0; not a supported Google
product. Status banner in its README: "**Production Ready**" with
"360 unit tests", but its own `docs/unimplemented_features.md`
flatly contradicts this (ownership keywords have no parser, contracts
have AST nodes but no parser, mutability not enforced, etc.). Treat
the README claims as marketing; treat the feature docs as design
notes for an aspirational system, partially backed by a real Rust /
LLVM compiler.

The point of this document is **not** to mock GA, it explored an
interesting design corner that Aether has largely ignored. The point is
to identify pieces of that design that would be **attractive to lift
into Aether** (lib-level or, if it pays off, language-level), and
to capture them at a level of detail sufficient to implement.

This survey draws on GA's `FINAL_DESIGN.md`,
`LANGUAGE_REFERENCE.md`, `docs/{enhanced_verification,
function_metadata, ownership_design, resource_management,
llm_optimized_errors, missing_features, unimplemented_features,
error_handling}.md`, the four representative `examples/*/main.aether`
that show actual code shape, the `stdlib/{io,concurrency}/*.aether`
surfaces, and `runtime/src/lib.rs` for the panic hook. Claims below
about what GA does or does not have rest on those sources.

---

## 1. The two languages at a glance

| Axis | Aether | GCP-Aether |
|---|---|---|
| Audience | Humans (with LLM assist) | "LLMs are the primary authors. Humans may read but never write", GA's words |
| Surface syntax | Go-ish: `func(x: int) -> int { … }` | S-expressions: `(DEFINE_FUNCTION (NAME 'f') (ACCEPTS_PARAMETER (NAME 'x') (TYPE INTEGER)) (RETURNS INTEGER) (BODY …))` |
| Backend | Compiles to C, then C compiler | LLVM IR via `inkwell`, written in Rust |
| Concurrency model | First-class actors (Erlang-shape: `actor`, `message`, `receive`, `send`) | Threads + mutex + channel as stdlib types; no language-level actor |
| Memory | Refcounted strings + arena-owned, drop-on-scope at codegen, manual at the C runtime layer | Designed: ownership tokens `^T` / `&T` / `&mut T` / `~T` à la Rust. Per GA's own `unimplemented_features.md`, **none of this is wired through**, keywords lex, AST nodes exist, parser is a stub, no semantic enforcement, no codegen. |
| Sandbox / capability | Three real layers, `--emit=lib` capability-empty default + `--with=fs,net,os` opt-in, `hide` / `seal except` lexical denylist, `libaether_sandbox.so` LD_PRELOAD libc-call gate | None. GA's stdlib has unconditional `std.io` / `std.net` / `std.concurrency`. No capability concept in the design. |
| Hosting other languages | `contrib.host.{lua,python,perl,ruby,tcl,js}.run_sandboxed(perms, code)` in-process; Java / Go / aether-host-aether out-of-process; `--emit=lib` produces ABI-stable `aether_<name>` exports for ctypes / Panama / Fiddle | None |
| Distinguishing strength | Real compiler with real users (svn-aether port). Capability discipline is genuinely original. | Verification metadata vocabulary, intent annotations, structured-error format, LLM-as-author framing |
| Distinguishing weakness | No formal verification or contracts story; ergonomics-only | Most of the headline features are docs-only; ownership system designed but never connected end-to-end |

The two languages are pointed in **different directions**: Aether is a
working systems language with a sandbox story, GA is a design
exercise about what an LLM-target language *should look like*. That
asymmetry is what makes the comparison interesting, there is nothing
to copy structurally, but there are individual ideas worth lifting.

---

## 2. Feature-by-feature

### 2.1 Surface syntax (GA's: extreme S-expression / Aether's: terse Go-ish)

GA's everything-is-a-keyword-S-expression shape:

```lisp
(DEFINE_FUNCTION
  (NAME 'add')
  (ACCEPTS_PARAMETER (NAME 'a') (TYPE INTEGER))
  (ACCEPTS_PARAMETER (NAME 'b') (TYPE INTEGER))
  (RETURNS INTEGER)
  (INTENT "Adds two integers")
  (BODY
    (RETURN_VALUE (EXPRESSION_ADD (VARIABLE_REFERENCE 'a') (VARIABLE_REFERENCE 'b')))))
```

Aether:

```aether,fragment
add(a: int, b: int) -> int { return a + b }
```

**Verdict, do not adopt.** GA's syntax is the keystone of the
"LLMs as authors" thesis: every node is fully named, parameters are
positional-by-keyword, there's "one way to express each concept".
For LLM generation this *might* improve guardrails, but for human
readability it's lost. Aether's tagline is "Go's ergonomics + Rust's
capability discipline + Erlang's actor syntax"; adopting the GA
S-expressions would torpedo the first leg. **Reject.**

### 2.2 INTENT strings on functions / modules / parameters

Every GA construct can carry an `(INTENT "free-text description")`
field. The compiler later does (claims to do) "intent analysis"
comparing the prose against detected behavior, with a confidence
score.

```lisp
(DEFINE_FUNCTION
  (NAME 'process_payment')
  (INTENT "Idempotent debit, retries via exponential backoff")
  ...)
```

**Verdict, partially attractive. Adopt as a doc-comment convention,
not a language feature.** Aether already uses `///` doc comments and
top-of-function prose. Formalizing an `@intent("…")` attribute that
flows into `std.docs` / `aether describe` and the LLM extension
context would cost nothing and be useful. The "compare prose to
behavior with confidence score" angle is vapor-grade in the GA repo
(the `intent_analysis` module is pattern-match heuristics, not
anything that would actually catch a bug); skip that part.

**Implementation sketch (stdlib + tooling):**

- New attribute `@intent("…")` recognized by the parser, attached to
  function / module / actor / struct AST nodes.
- Stored in the per-symbol metadata table that already feeds
  `--emit=lib`'s `aether_describe()` output.
- `aether describe` (already exists) gains an `--intent` flag to
  print intent fields alongside signatures.
- LSP hover surfaces it.
- **No** "intent verification" pass; that's a research project not
  worth funding here.

### 2.3 Preconditions / postconditions / invariants (Eiffel-style contracts)

GA's design specifies:

```lisp
(DEFINE_FUNCTION
  (NAME safe_divide)
  (ACCEPTS_PARAMETER (NAME numerator) (TYPE FLOAT))
  (ACCEPTS_PARAMETER (NAME denominator) (TYPE FLOAT))
  (RETURNS FLOAT)
  (PRECONDITION
    (PREDICATE_NOT_EQUALS denominator 0.0)
    (FAILURE_ACTION THROW_EXCEPTION)
    (PROOF_HINT "denominator != 0 is checked before division"))
  (POSTCONDITION
    (PREDICATE_EQUALS RETURNED_VALUE (EXPRESSION_DIVIDE numerator denominator))
    (PROOF_HINT "Result is mathematically correct division"))
  (BODY
    (RETURN_VALUE (EXPRESSION_DIVIDE numerator denominator))))
```

Failure actions: `ASSERT_FAIL` (panic), `LOG_WARNING`, `THROW_EXCEPTION`.
The plan was Z3 SMT integration for compile-time discharge, runtime
checks otherwise. Per `enhanced_verification.md` and
`unimplemented_features.md`, the SMT solver is a stub interface and
the parser doesn't actually parse the contracts inside function
defs, they live in lexer + AST + a verifier-shaped Rust module
that's never reached.

**Verdict, adopt the runtime-checked subset; defer SMT.**

This is the single most LLM-attractive idea in the GA design that
Aether does not already have. Aether's `assert(...)` macro exists but is
positional and untyped in terms of contract role. A pre/post pair
attached to a function declaration is a **better surface** for LLMs
to generate than scattered asserts at the top and bottom of the body,
because the metadata stays attached to the signature and survives in
the doc / lib export.

The shape worth building:

```aether,fragment
add(a: int, b: int) -> int
    requires a >= 0
    requires b >= 0
    ensures result >= a, result >= b
{
    return a + b
}
```

- **Lex/parse:** `requires` / `ensures` / `invariant` are reserved
  words (none of these collide with current keyword table per a
  grep). They appear after the `-> T` and before the `{`. Comma-
  separated list of boolean expressions. `result` is bound inside
  `ensures` to the return value (compiler emits an alias to the
  return slot).
- **Lowering:** `requires` lowers to a runtime check at function entry
  with `panic("precondition_violation: <expr> in <fn>")` on false.
  `ensures` lowers to the same, but at every `return` statement; the
  compiler walks the body and inserts the check before each return
  (and at function-end for void/falloff).
- **Failure mode:** a single mode, `panic`. Skip the
  `LOG_WARNING` / `THROW_EXCEPTION` enum; Aether has no exceptions,
  panics are the established failure path, and adding three modes
  costs design surface for no real win.
- **Compile-time eval:** when both sides of a `requires` are constant-
  foldable, evaluate at compile time and emit a diagnostic on guaranteed
  failure (mirrors how `byte b = 256` is rejected at compile time per
  CHANGELOG 0.108.0).
- **Disable flag:** `--no-contracts` for release builds that want
  to skip the runtime checks (compiler emits the precondition checks
  conditionally on a build flag, like assertions in C).
- **Interaction with `--emit=lib`:** preconditions on exported
  functions stay in the `aether_describe()` output as part of the
  signature metadata, so Python ctypes / Java Panama bindings can
  see them.
- **No SMT.** Not now, possibly never. The combinatorial explosion
  of "discharge contracts at compile time" is a research project
  with a 10-year horizon; runtime-checked contracts have been a
  shipping feature in Eiffel and D for decades and pay back at the
  level that matters here (catching bugs, communicating intent, giving
  LLMs a target shape).

**Effort estimate:** 3-5 days. Lexer + parser + an entry-block check
emit + a per-return-rewrite pass + a compile-time const-fold rejector.
Test surface: `tests/regression/test_requires_ensures.ae`,
`tests/integration/contract_violation_at_runtime/`.

**Where this lands:** a new P2 backlog entry, ahead
of the speculative fs.realpath etc. The reason it ranks: every
downstream user writes asserts; this is a strict ergonomic
improvement to a thing they already do. The svn-aether port
specifically has hand-written `if (!(cond)) panic("…")` shapes that
this would replace 1:1.

### 2.4 Behavioral spec annotations (`(IDEMPOTENT TRUE)`, `(PURE TRUE)`, `(SIDE_EFFECTS …)`)

```lisp
(BEHAVIORAL_SPEC
  (IDEMPOTENT FALSE)
  (PURE FALSE)
  (SIDE_EFFECTS
    (MODIFIES "payment_database")
    (SENDS "email_notification"))
  (TIMEOUT_MS 5000)
  (RETRY_POLICY "exponential_backoff"))
```

**Verdict, partially attractive but mostly cosmetic. Pick three.**

The full grid (idempotent, pure, deterministic, thread_safe,
may_block, side-effects, timeout, retry policy, exception safety,
purity spec) is too much. But three of these have real bite:

- **`@pure`**, useful for the optimizer and for memoization. It
  could mark stdlib functions that are safe to constant-
  fold in `const X = …` contexts (recall: 0.110.0 closed the silent-
  wrong-results trap for `const X = some_function_call()`).
  `@pure` would be the explicit allow-list mechanism, `const X =
  some_pure_fn(arg)` becomes legal if the function is `@pure` and
  args are const.
- **`@thread_safe`**, useful for actor reasoning. Marking a function
  thread-safe means it's callable from multiple actors without lock-
  ing; the compiler can warn when a non-thread-safe function is
  called from inside a `receive` block.
- **`@may_block`**, useful in actor handlers. A blocking call inside
  a `receive` is a known footgun; an explicit annotation lets the
  compiler emit a warning when one is called without `spawn` /
  `await` / a worker actor pattern (see `aether-embedded-in-host-
  applications.md` / `structured-concurrency.md`).

Skip `idempotent`, `deterministic`, `timeout_ms`, `retry_policy`,
`side_effects.modifies`, `exception_safety`. These are documentation,
not type-system, and doc comments already cover them.

**Implementation sketch:**

- Three new attributes: `@pure`, `@thread_safe`, `@may_block`.
- Stored in symbol metadata.
- `@pure` interacts with the const-eval path (typechecker.c
  `is_const_evaluable_call`).
- `@may_block` adds a warning class to the actor body checker
  (compiler/analysis/actor_checker.c, exists).
- Per `--emit=lib`, the three attributes flow into `aether_describe()`.

**Effort estimate:** 1 day for the attributes + plumbing, plus ~half
a day each for the three real interactions. Call it 3 days.

### 2.5 RESOURCE_SCOPE (RAII with explicit cleanup-fn declaration)

```lisp
(RESOURCE_SCOPE
  (NAME "file_op")
  (ACQUIRE_RESOURCE
    (RESOURCE_TYPE "file_handle")
    (RESOURCE_BINDING "f")
    (VALUE (CALL_FUNCTION (NAME "open_file") (ARGUMENT "data.txt")))
    (CLEANUP "file_close"))
  (CLEANUP_GUARANTEED TRUE)
  (CLEANUP_ORDER REVERSE_ACQUISITION)
  (BODY
    ;; use f
    ))
```

Cleanup orderings: `ReverseAcquisition` (LIFO, default), `ForwardAcquisition`,
`DependencyBased`, `Parallel`. Plus a `RESOURCE_CONTRACT (MAX_FILE_HANDLES 10)
(MAX_MEMORY_MB 100)` form for runtime-enforced limits.

**Verdict, already covered by `defer`; do not adopt the construct.
But the `RESOURCE_CONTRACT` runtime-limits idea is interesting.**

Aether's `defer` (per `docs/language-reference.md` §8) is the LIFO-
cleanup-on-scope-exit construct, and it composes cleanly with normal
expressions:

```aether,fragment
f, err = fs.open("data.txt", "r")
if err != "" { return err }
defer fs.close(f)
// use f
```

That's strictly more readable than the GA shape, gets the same
"cleanup guaranteed" semantics, and doesn't need a new keyword.
Skip `RESOURCE_SCOPE`.

The **runtime resource limit** part is genuinely novel and has no
counterpart in Aether's design so far. The shape:

```aether,fragment
process_request(req: Request) -> string
    @max_open_fds(10)
    @max_memory_mb(100)
    @max_time_ms(5000)
{
    // body
}
```

This pairs naturally with the actor model, a per-actor budget that
trips `panic("budget exceeded: max_open_fds=10")` or sends a `BudgetExceeded`
message when the cap is hit. It also pairs with `--emit=lib`: a host
embedding Aether-as-DSL wants to cap untrusted scripts.

**Verdict on this sub-feature, attractive enough to file as a P3
on the backlog.** Not P1: Aether lacks the runtime tracking
infrastructure (no per-fd accounting, no memory accounting outside
the arena). It's the kind of thing that wants the LD_PRELOAD
sandbox first as the substrate, then `@max_open_fds` is a thin
declaration on top.

**Implementation sketch (deferred, sketchy):**

- Per-thread / per-actor budget counters in the runtime.
- libaether_sandbox.so already intercepts `open`, `socket`, etc.,
  extend the hook to bump a per-budget counter.
- Compiler emits a budget-scope-enter / budget-scope-exit at function
  entry / exit.
- Failure: panic with the budget name and the call that pushed over.

This is several weeks of work and has prerequisites; **defer** until
a concrete user asks.

### 2.6 Ownership system (`^T`, `&T`, `&mut T`, `~T`)

GA's docs describe Rust-shape ownership with four kinds: owned,
borrowed-immut, borrowed-mut, refcounted-shared. Per GA's own
`docs/unimplemented_features.md`:

> Keywords exist but no implementation at any level. AST has full
> support for ownership/lifetime concepts. Parser has KeywordType
> enum entries but no parsing logic. No semantic analysis for
> ownership tracking. No MIR lowering for ownership semantics. No
> LLVM codegen for ownership.

**Verdict, explicitly reject.** This is the headline gap between
GA's stated goals and what was built; copying it would be copying
the most-broken part of the project. More importantly, the design
direction is wrong for Aether. The `LLM.md` guidance is unambiguous:

> NOT Rust, no borrow checker, no ownership, no lifetimes. Strings
> are ref-counted or arena-owned; you release explicitly where it
> matters, drop-on-scope-exit elsewhere.

Pony-style capabilities, *not* Rust ownership, are the Aether model. This
isn't a "missing feature"; it's an explicit design rejection
that has already paid back (no borrow-checker user-friction, codegen
stays simple, FFI is direct). Move on.

### 2.7 Structured / LLM-optimized error format

GA's compiler emits errors as S-expressions:

```lisp
(COMPILATION_ERROR
  (ERROR_CODE "TYPE-001")
  (SEVERITY "ERROR")
  (LOCATION (FILE "math.aether") (LINE 42) (COLUMN 12))
  (MESSAGE "Type mismatch: expected Float, found Integer")
  (EXPLANATION "The expression has type 'Integer' but type 'Float' was expected")
  (FIX_SUGGESTION_1
    (DESCRIPTION "Convert integer to float")
    (CONFIDENCE 0.95)
    (ADD_CODE "(CAST_TO_TYPE value FLOAT)")))
```

With per-error codes (`SEM-001`, `TYPE-042`), severity levels,
auto-fix suggestions with confidence scores, related diagnostics,
intent-mismatch flags, and a `PARTIAL_COMPILATION_RESULT` envelope
that says "these modules built, these didn't, here's the executable
anyway".

**Verdict, adopt the error-code idea; skip the rest.**

What works:
- **Stable error codes.** `AETHER-E0042: …`, addressable by URL.
  These are useful for documentation cross-linking and for the LSP /
  editor extension to show a "more info" link. Aether should have them.
- **Machine-parseable error output mode.** `aetherc --diagnostic-format=json`
  emitting one JSON object per diagnostic. This is the right shape
  for editor integration, LLM tool-use loops, and CI annotation
  bots. Aether already emits human-readable output; adding a JSON mode
  is small and well-bounded.

What doesn't work:
- **Auto-fix suggestions with confidence scores**, every existing
  attempt at this in mainstream compilers (Rust, TypeScript, Swift)
  is hand-curated per-error-kind. Building a generic infrastructure
  for it is overhead until specific high-frequency errors exist
  to target. Defer.
- **Intent mismatch detection**, needs the `@intent` infra first
  (see 2.2), and even then is heuristic. Skip.
- **`PARTIAL_COMPILATION_RESULT`**, doesn't match how the Aether
  compiler works (single-pass, errors are fatal at the module level). Skip.

**Implementation sketch:**

- `compiler/aether_diagnostics.c` already centralizes error
  emission. Add a stable code per error (`AETHER-E0001` through
  `AETHER-E0NNN`); pick a registry file `compiler/error_codes.txt`
  with the code + short message + URL fragment.
- Add `--diagnostic-format=json` flag; emit one
  `{"code": "AETHER-E0042", "severity": "error", "file": …, "line": …, "col": …, "message": …, "context_lines": [...]}`
  per diagnostic to stderr.
- Editor extension (already exists) consumes the JSON and renders
  inline.

**Effort estimate:** 2 days for the JSON output, plus an ongoing
effort to retrofit codes onto existing diagnostics (background work,
opportunistic).

**Where this lands:** a new P2 backlog entry, paired with
the contract feature since both want stable codes for the new
diagnostics they introduce.

### 2.8 Custom panic handler with stack traces (their `error_handling.md`)

GA's runtime installs `std::panic::set_hook` and prints a structured
panic report with a filtered backtrace before exiting:

```text
Error: AetherScript Runtime Panic
Reason: Precondition violation: 'denominator must not be zero'
Location: src/stdlib/math.aether:15:8

Stack Trace (most recent call first):
  0: math::safe_divide
      at src/stdlib/math.aether:15
  1: main::calculate_value
      at src/main.aether:25
  2: main
      at src/main.aether:5
```

Filter strips Rust internals (`std::`, `core::`, `backtrace::`,
`rust_begin_unwind`), leaves only AetherScript-relevant frames,
demangles via `rustc_demangle`, exits 101.

**Verdict, strongly attractive. Adopt with the Aether toolchain
(libunwind + symbolization), not GA's (Rust panic hook).**

Aether currently panics via `aether_panic_handler` in
`compiler/runtime/aether.h` (or wherever, check) which prints the
message and aborts. No stack trace. For the actor model and for
contract violations (if 2.3 lands), a filtered stack trace is high
value: it's the difference between "your program crashed" and "your
program crashed in `actor::Calculator::handle_message → math.safe_divide
at math.ae:15`."

**Implementation sketch:**

- New runtime file `runtime/aether_backtrace.c`. POSIX:
  `backtrace(3)` + `backtrace_symbols(3)` (glibc) or libunwind
  (better, portable). Windows: `CaptureStackBackTrace`.
- Demangling is unnecessary, Aether generates plain C with stable
  names, so `aether_<module>_<function>` is already readable. The
  pretty-printer can strip the `aether_` prefix and turn
  underscores back into dots (`aether_std_string_concat` →
  `std.string.concat`).
- Filter rule: drop frames whose symbol is in libc, the runtime
  internals (`aether_runtime_`, `aether_actor_runtime_`), and the
  panic handler itself.
- Hook: install in `aether_runtime_init()` (already exists per
  `compiler/runtime/aether.c`), called from the synthesized `main()`
  prologue.
- Build: gate on `-rdynamic` so the symbol table is in the binary
  (release builds with strip should fall back to addresses-only
  output with a "build with `--debug-symbols` for names" hint).
- `--emit=lib`: the panic handler should be installable but not
  default, the host process owns its panic policy. Add
  `aether_install_panic_handler()` as an exported function.

**Effort estimate:** 2-3 days for POSIX; another day for Windows.

**Where this lands:** P1 on the backlog, alongside contracts
(2.3) since the panic from a contract violation is the place the
trace earns its keep.

### 2.9 Resource contracts as runtime-enforced limits

Already covered as a sub-bullet under 2.5. `@max_memory_mb`,
`@max_time_ms`, `@max_open_fds` per-function declarations. Defer.

### 2.10 Performance / complexity expectation annotations

```lisp
(performance_expectation LATENCY_MS 0.01 "99th percentile")
(complexity_expectation TIME BIG_O "n log n")
(algorithm_hint "binary search")
```

**Verdict, reject as a language feature. Maybe useful as benchmark
metadata.** These are hand-asserted documentation; nothing in the
GA compiler verifies them (and the closed-form verification of "is
this actually O(n log n)" is undecidable). At best they're harness
input for a benchmarking tool that warns when measured perf drifts
from declared. Aether already has `benches/`; were this ever wanted,
it would live as a YAML sidecar in the bench harness, not as a language
attribute. Skip.

### 2.11 Verified / typed semantic types (`(DEFINE_SEMANTIC_TYPE …)`)

```lisp
(DEFINE_SEMANTIC_TYPE
  (NAME "email_address")
  (BASE_TYPE STRING)
  (CONSTRAINT (MATCHES_REGEX "^[a-zA-Z0-9._%+-]+@…"))
  (SEMANTIC_CATEGORY "contact_information")
  (PRIVACY_LEVEL "personally_identifiable"))
```

**Verdict, reject.** This is the "newtype with validation" pattern.
It's a real design space (refinement types, F#-style units of
measure), but the cost/value for the Aether user base is poor. Aether
projects that want it can write a struct wrapper:

```aether,fragment
struct EmailAddress { raw: string }
parse_email(s: string) -> (EmailAddress, string) { … }
```

…with the validation in the parser fn. The language stays small.

### 2.12 Stdlib comparison (GA's: aspirational; Aether's: real and growing)

GA's stated stdlib (its `docs/standard_library.md`):
- `std.io` file ops, console, list_directory
- `std.collections` sort, binary_search, filter, map, reduce
- `std.math` safe_add, safe_multiply, sqrt, sin/cos/tan, log, exp
- `std.time` timestamp / datetime / duration
- `std.net` tcp + http_get/post
- `std.concurrency` thread / mutex / channel / atomic_int

Most of this is `.aether` source declaring the surface plus stub Rust
implementations in `runtime/src/`. The HTTP server example is the
one part that demonstrably works.

Aether (per `docs/stdlib-reference.md` and `std/`):
`std.{actors, bytes, collections, config, cryptography, dir, dl, file,
fs, host, http, intarr, io, json, list, log, map, math, net, os, path,
string, tcp, zlib}`, and more contrib. Real, tested, used by
downstream svn-aether.

**Verdict, GA has nothing Aether needs.** If anything, the comparison
runs the other way: GA's `std.io` is a strict subset of Aether's (which
has fd-level surface, glob, atomic write, binary-safe read, etc.).
GA's `std.concurrency` is a thread+mutex shape; Aether has first-class
actors, a higher abstraction. GA's `std.net` is an HTTP-
client + raw-TCP shape; Aether has `std.http` server + `std.tcp`.

**One caveat:** GA's `std.time` has a more developed `datetime` /
`duration` / ISO-8601 model than Aether's current `std.time` (which is
mostly Unix-timestamp arithmetic, confirm with a quick read). If
Aether's `std.time` is in fact thinner, the GA module shape is a
reasonable reference for what to add: `datetime` struct, `duration`
struct with nanosecond precision, ISO-8601 parse/format, timezone
offset awareness. Worth a separate audit, but file as P3.

### 2.13 Generation hints (`(GENERATION_HINTS (PREFER_STYLE …))`)

```lisp
(GENERATION_HINTS
  (FOR_PATTERN "database_operation")
  (PREFER_STYLE "defensive")
  (INCLUDE_PATTERNS ("null_checking" "transaction_wrapping"))
  (AVOID_PATTERNS ("direct_sql" "string_concatenation")))
```

**Verdict, reject.** Belongs in editor / LLM extension config, not
in the language. Aether's `editor/` already has VS Code config; if
LLM-style nudges are wanted, that's the layer.

### 2.14 Verified pattern library / DEFINE_PATTERN

GA's design includes a way to declare reusable patterns (request-
response handler, transactional operation, etc.) that the LLM can
target as a building block. None of it is implemented. This is the
same idea as Rust macros + Eiffel design-by-contract had a child.

**Verdict, reject.** Aether has closures, builder DSLs, and trailing
closures (per `docs/closures-and-builder-dsl.md`). The composability
is already there at a higher level. Adding a "PATTERN" abstraction
on top is a layer of indirection without a payoff.

---

## 3. Things GA has that are worth genuine consideration, summary table

| # | Feature | Verdict | Effort | Backlog placement |
|---|---|---|---|---|
| 2.2 | `@intent("…")` attribute (no verification) | Adopt | 0.5 day | P3 |
| 2.3 | `requires` / `ensures` runtime-checked contracts | **Adopt, strong fit** | 3-5 days | **P2** |
| 2.4 | `@pure` / `@thread_safe` / `@may_block` (just these three) | Adopt | 3 days | P3 |
| 2.7 | Stable error codes + `--diagnostic-format=json` | Adopt | 2 days | P2 |
| 2.8 | Filtered runtime stack trace on panic | **Adopt, strong fit** | 2-3 days POSIX, +1 day Windows | **P1** |
| 2.5 (sub) | `@max_open_fds` / `@max_memory_mb` runtime budgets | Defer | weeks (needs sandbox plumbing) | P4 |
| 2.12 | `std.time` datetime/duration/ISO-8601 audit | Investigate | TBD | P3 (audit) |

## 4. Things GA has that Aether should explicitly reject

| # | Feature | Why reject |
|---|---|---|
| 2.1 | S-expression syntax | Costs the human-readable leg of the value prop |
| 2.4 (rest) | `@idempotent`, `@deterministic`, `@timeout_ms`, `@retry_policy`, etc. | Documentation, not type system; doc comments already cover it |
| 2.5 | `RESOURCE_SCOPE` construct | `defer` already covers this with less ceremony |
| 2.6 | Rust-shape `^T` / `&T` / `&mut T` ownership | Explicit design rejection in Aether's `LLM.md`; GA's own impl is a stub |
| 2.7 (rest) | Auto-fix suggestions, intent-mismatch detection, partial compilation | High infra cost, low payoff at Aether's scale |
| 2.10 | Performance / complexity expectation attributes | Hand-asserted docs; cannot be verified |
| 2.11 | `DEFINE_SEMANTIC_TYPE` / refinement types | Wrapper struct + parser fn covers the use case |
| 2.13 | Generation hints baked into language | Belongs in editor/LLM extension config |
| 2.14 | Verified pattern library | Closures + builder DSL already cover the composability need |

## 5. Things that don't translate at all

GA was designed around **LLMs as authors**. A meaningful chunk of
its feature surface (extreme S-expression syntax, intent verification,
generation hints, pattern templates, `PARTIAL_COMPILATION_RESULT`,
auto-fix-with-confidence) only makes sense under that thesis. Aether's
thesis is **humans as authors with LLM assist**, the design center
is human ergonomics and the LLM gets the same surface humans do, no
special accommodations.

That asymmetry means **most of GA isn't useful here**, including
the parts GA treated as most distinctive. The pieces worth
lifting are the ones that happen to be useful regardless of authorship
model: contracts (2.3) and stack traces (2.8) help any developer
catch any bug. Error codes (2.7) help any IDE integrate with any
compiler. The intent annotation (2.2) is a doc-comment convention
that's nice to have but not load-bearing.

---

## 6. Concrete proposed backlog additions

```diff
+ ## P1, runtime panic stack traces
+
+ When `aether_panic_handler` fires (contract violation, explicit
+ `panic("…")`, actor crash, division by zero, etc.), capture and
+ print a filtered, demangled call stack before exiting. POSIX uses
+ libunwind; Windows uses `CaptureStackBackTrace`. Filter rule: drop
+ libc, runtime internals, panic handler itself. Symbol pretty-printer
+ turns `aether_std_string_concat` into `std.string.concat`. New file:
+ `runtime/aether_backtrace.c`. Build flag: `-rdynamic` for symbol
+ table availability. `--emit=lib` exports an installer
+ `aether_install_panic_handler()` so embedding hosts can opt in.
+
+ ## P2, `requires` / `ensures` contracts (runtime-checked)
+
+ Eiffel-style preconditions and postconditions on function declarations,
+ runtime-checked, panic on violation. Surface:
+
+     fn(x: int) -> int requires x >= 0 ensures result >= x { … }
+
+ `result` is bound inside `ensures` to the return value. Both clauses
+ are comma-separated boolean expressions. Compile-time const-fold
+ rejects guaranteed failures. `--no-contracts` disables emission for
+ release builds. Metadata flows through `--emit=lib`'s
+ `aether_describe()` so FFI consumers see the contracts. No SMT, no
+ `@invariant` (yet), no `LOG_WARNING` / `THROW_EXCEPTION` modes, just
+ panic. Reference: GoogleCloudPlatformAether_COMPARISON.md §2.3.
+
+ ## P2, stable error codes + `--diagnostic-format=json`
+
+ Every diagnostic gets a stable code (`AETHER-E0042`). Registry file
+ `compiler/error_codes.txt`. New CLI flag `--diagnostic-format=json`
+ emits one JSON object per diagnostic. Editor extension consumes the
+ JSON. Codes back-fill onto existing diagnostics opportunistically.
+ Reference: GoogleCloudPlatformAether_COMPARISON.md §2.7.
+
+ ## P3, `@intent("…")`, `@pure`, `@thread_safe`, `@may_block` attributes
+
+ Four function-level attributes. `@intent` is documentation surfaced in
+ LSP hover and `aether describe` (no verification). `@pure` interacts
+ with the const-eval allowlist (a `@pure` function called with const
+ args becomes legal in `const X = …`). `@thread_safe` and `@may_block`
+ feed the actor body checker, calling a `@may_block` function inside
+ a `receive` without `spawn` is a warning. Reference:
+ GoogleCloudPlatformAether_COMPARISON.md §2.2, §2.4.
+
+ ## P3, `std.time` audit against datetime/duration/ISO-8601 shape
+
+ Read GoogleCloudPlatform/Aether's `std.time` design (`datetime` struct,
+ `duration` with nanosecond precision, `format_iso8601` /
+ `parse_iso8601`, timezone offset awareness) and compare against Aether's
+ current `std.time`. If it is meaningfully thinner, build the missing
+ pieces. Reference: GoogleCloudPlatformAether_COMPARISON.md §2.12.
+
+ ## P4, runtime resource budgets (`@max_open_fds`, `@max_memory_mb`)
+
+ Per-function (or per-actor) runtime-enforced budget caps. Trip
+ panics or send a `BudgetExceeded` message when hit. Wants the
+ libaether_sandbox.so substrate as a prerequisite (extend its libc
+ hooks to bump per-budget counters). Defer until a concrete user
+ asks. Reference: GoogleCloudPlatformAether_COMPARISON.md §2.5.
```

---

## 7. Closing note

Their FINAL_DESIGN.md says:

> The future of programming is not humans writing code, but humans
> expressing intent and LLMs generating verified, correct
> implementations.

Aether takes the opposite view. The future of programming is humans
writing code with LLM assistance, humans still need to read, debug,
modify, and reason about the code. A language designed exclusively
for LLM generation throws away the readability that makes software
maintainable. That said, several of GA's concrete language-feature
ideas (contracts, stack traces, error codes, attributes) are useful
**regardless of authorship model**, and worth picking up.

The headline-feature delta in the other direction is bigger: Aether's
sandbox / capability story (`--emit=lib` + `--with=` + `hide` /
`seal except` + LD_PRELOAD libc gate), its actor model, and its
host-other-languages framework have no analog in GA. GA went deep
on metadata; Aether went deep on what the binary actually does. Both
choices are coherent; Aether's has shipping users.

---

## TL;DR for triage

The full doc above compares Aether against [GoogleCloudPlatform/Aether](https://github.com/GoogleCloudPlatform/Aether) ("GA" or "GCP-Aether" in the doc), a stalled exploration repo from Google that happens to share this project's name. GA appeared later than Aether. License Apache-2.0, not a supported Google product, design-doc-heavy with much of the headline functionality unimplemented per GA's own `unimplemented_features.md`.

This survey is the fifth in a series (sister umbrellas: Flux, Fir, Flint, Zym). It differs from the others in shape: GCP-Aether's design center is **"LLMs as authors"** vs Aether's **"humans with LLM assist"**. That asymmetry means most of GA's distinctive features (extreme S-expression syntax, intent verification, generation hints, pattern templates, partial-compilation envelopes) don't translate. The pieces worth lifting are the ones useful **regardless of authorship model**, contracts, stack traces, error codes, attributes.

**Tone note**: the comparison doc is deliberately kind, GCP-Aether "explored an interesting design corner that Aether has largely ignored." Should Aether's maintainers ever approach GA's maintainers about the name collision (Aether came first, GA is stalled), the framing should stay collaborative rather than competitive.

## The doc's verdict table (§3 + §4)

**Adopt, strong fit:**

| § | Feature | Effort | Priority |
|---|---|---|---|
| 2.8 | Filtered runtime stack trace on panic | 2-3 days POSIX, +1 Windows | **P1** |
| 2.3 | `requires` / `ensures` runtime-checked contracts | 3-5 days | **P2** |
| 2.7 | Stable error codes + `--diagnostic-format=json` | 2 days | P2 |
| 2.4 (subset) | `@pure` / `@thread_safe` / `@may_block` only | 3 days | P3 |
| 2.2 | `@intent(...)` attribute (no verification) | 0.5 day | P3 |
| 2.5 (sub) | `@max_open_fds` / `@max_memory_mb` runtime budgets | weeks (needs sandbox plumbing) | P4 |
| 2.12 | `std.time` audit vs datetime/duration/ISO-8601 | TBD audit | P3 |

**Reject:**

| § | Feature | Why |
|---|---|---|
| 2.1 | S-expression syntax | Costs the human-readable leg of the value prop |
| 2.4 (rest) | `@idempotent`, `@deterministic`, `@timeout_ms`, `@retry_policy`, etc. | Documentation, not type system; doc comments already cover it |
| 2.5 | `RESOURCE_SCOPE` construct | `defer` already covers this with less ceremony |
| 2.6 | Rust-shape `^T` / `&T` / `&mut T` ownership | Explicit design rejection in Aether's LLM.md; GA's own impl is a stub |
| 2.7 (rest) | Auto-fix suggestions, intent-mismatch detection, partial compilation | High infra cost, low payoff at Aether's scale |
| 2.10 | Performance / complexity expectation attributes | Hand-asserted docs; cannot be verified |
| 2.11 | `DEFINE_SEMANTIC_TYPE` / refinement types | Wrapper struct + parser fn covers the use case |
| 2.13 | Generation hints baked into language | Belongs in editor/LLM extension config |
| 2.14 | Verified pattern library | Closures + builder DSL already cover composability |

**Following the prior umbrella pattern:** the two strongest-fit Tier-1 items have been filed as focused issues, the P1 panic-stack-trace work and the P2 `requires`/`ensures` contracts. The other adopt-list items (error codes, attributes, time audit, runtime budgets) are smaller / depend on other things first; each is a candidate for an individual issue once greenlit.

## Comparison vs the four prior surveys

The five comparison docs each emphasise a different design axis:

- **Flux**: bit-precise types (`data{N}`), binary protocol parsing
- **Fir**: row-typed errors, `#[derive(...)]`, anonymous records, type-system ergonomics
- **Flint**: optionals, variants, FIP C-bindings, runtime-shape ergonomics
- **Zym**: nested-VM sandboxing, embedder-shape extensions
- **GCP-Aether** (this survey): contracts, stack traces, attributes, debuggability + LLM-target metadata

The closest overlap is with the Pollen `--emit=lib` issues (resource caps, caller info), GCP-Aether's runtime-budget proposal (§2.5) is a similar shape to Pollen's resource caps but at the function-attribute layer rather than the host-side setter. Worth considering them together if/when the runtime accounting infrastructure lands.

## What this survey is NOT asking

- Not a roadmap. "Use this as a menu, not a roadmap."
- Not asking for any single feature to ship as-listed.
- Not litigating the rejection list, those are correctly identified as wrong-fit for Aether's identity.
- Not making a name-collision claim. Aether came first; the GA repo is stalled. If and when Aether's maintainers approach GA about the name, that's a separate conversation, not part of this technical comparison.

## Cross-refs

- Source project: [GoogleCloudPlatform/Aether](https://github.com/GoogleCloudPlatform/Aether); the survey material is captured above for permanence.
- Sister surveys: Flux, Fir, Flint, Zym
- Sibling `--emit=lib` work: Pollen resource caps and caller-info
- Filed as focused issues: P1 panic stack traces (separate), P2 contracts (separate)
