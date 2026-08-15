# Mutation testing — `std.mutation`

Mutation testing measures **how good your tests are**. It changes the code
under test (the "SUT") one tiny edit at a time — `+` becomes `-`, `>` becomes
`<` — and re-runs your test suite for each change. A change your tests *catch*
is a **killed** mutant (good). A change that slips through is a **survivor** —
proof your tests have a gap.

`std.mutation` is a small library (one entry point, `mutation.run`) with a
runnable front-end at `examples/mutation-testing/mutate.ae`. It was adopted
from the sunsetting aeocha repo's `contrib/mutate` and uses `std.spec` purely
as an **oracle**, through the structured-report contract (`AE_SPEC_FORMAT` /
`AE_SPEC_REPORT`, docs/testing.md).

## Why it's an outer driver (not a test helper)

The compiler has already run by the time any test executes, so a mutant can't
be produced in-process — there's no source or AST left to perturb, only
compiled machine code. Mutation therefore has to, once per mutant:

1. edit the SUT **source on disk** and clear the build cache,
2. **`ae check`** the test — if the mutant doesn't type-check, it's *no-compile*
   (excluded from the score; a mutant that won't build was never really tested),
3. **`ae build`** the test to a binary and run it with
   `AE_SPEC_FORMAT=aeocha` + `AE_SPEC_REPORT=<file>` set, **reading the
   structured report** from that file rather than scraping stdout. The report's
   `failed=N` is the verdict: `N > 0` → killed, `N == 0` → survived.

`mutation.run` is that outer loop; your test file is completely unaware
mutation is happening. Using the structured report (not just a process exit
code) is what lets the tool tell *killed* (a test actually failed) apart from
*no-compile* (the mutation produced invalid code) — so a non-compiling mutant
never inflates the score by masquerading as a kill.

> Compiler note: the no-compile gate greps `ae check`'s output for an `error[`
> diagnostic rather than trusting its exit code. On the current `ae`, both
> `ae check` and `ae build` can print a compile error to stderr yet still exit 0
> (and `build` will even emit a binary linked against a stale module) — see
> issue #953. Grepping the diagnostic is the reliable signal until that's fixed.

## Usage

```bash
ae run examples/mutation-testing/mutate.ae -- <sut.ae> <test.ae> [lib_dir]
```

`lib_dir` is a module search dir handed to the per-mutant sub-builds via
`AETHER_LIB_DIR`; it defaults to the SUT's directory (which is usually right —
the test's `import <sut-module>` resolves there). The `ae` used for the
sub-builds is `ae` on PATH, overridable with the `AE_BIN` environment variable
(the in-tree regression harness points it at `build/ae`).

Or from a program / build script:

```
import std.mutation

survivors = mutation.run(sut_path, test_path, "")
// survivors >= 0: run completed (0 = every compiling mutant killed)
// survivors < 0:  aborted (unreadable SUT, or baseline suite fails)
```

Worked example (from the repo root):

```bash
ae run examples/mutation-testing/mutate.ae -- \
    examples/mutation-testing/lib/calc.ae \
    examples/mutation-testing/lib/calc_test.ae
```

`examples/mutation-testing/lib/` holds `calc.ae` (the code under test) plus
`calc_test.ae` (its `std.spec` suite). Mutation testing asks how well that
suite tests that code. Output:

```
Aether mutation testing (std.mutation)
  SUT:  examples/mutation-testing/lib/calc.ae
  test: examples/mutation-testing/lib/calc_test.ae

  baseline: suite passes on unmutated SUT ✓

  killed     calc.ae:8  ADD->SUB
  killed     calc.ae:11  SUB->ADD
  killed     calc.ae:11  LT->GT

  3/3 mutants killed — mutation score 100%
```

100% — every single-operator change to `calc.ae` was caught by `calc_test.ae`.
Each line is one mutant, anchored to its source location: `ADD->SUB` flips the
`+` in `add`; `SUB->ADD` flips the `0 - n` in `abs`; `LT->GT` flips the
`n < 0` guard. The suite asserts both branches of `abs` and an addition with a
negative, so all three are killed.

### What a survivor looks like

A survivor is the interesting case — it's a *test gap*. Delete the
negative-input case from `calc_test.ae` (the `abs(-7)` assertion), and the
`SUB->ADD` mutant suddenly survives:

```
  killed     calc.ae:8  ADD->SUB
  SURVIVED   calc.ae:11  SUB->ADD
  killed     calc.ae:11  LT->GT

  2/3 mutants killed — mutation score 66%
  1 survived (test gaps):
    - calc.ae:11  SUB->ADD
```

`SUB->ADD` flips the `0 - n` in `abs` to `0 + n` — which only changes the
result for a *negative* input. With `abs(-7)` removed, nothing feeds `abs` a
negative, so the mutant goes unnoticed. (Note `LT->GT` is still killed:
flipping the `n < 0` guard sends the positive `abs(7)` down the negation
branch, and that case is still tested.) Add the negative-input assertion back
and `SUB->ADD` returns to killed. That is the whole point — survivors are your
missing tests.

## Mutation operators (core set)

Operator mutators are matched **whitespace-padded** (`" + "`, `" > "`), so
normal Aether spacing is required for a site to be seen — and a padded
operator that sits *inside a string literal* is skipped (the tool tracks
string boundaries, so `"a + b"` in your code is never mutated as arithmetic).

| Operator | Mutation |
|---|---|
| `+` ↔ `-` | arithmetic |
| `*` → `/` | arithmetic |
| `>` `<` `>=` `<=` | comparison flips |
| `==` ↔ `!=` | equality flips |
| `&&` ↔ `\|\|` | boolean |

String-literal mutators target the literal's *content* (quotes preserved):

| Mutator | Mutation |
|---|---|
| `STR->EMPTY` | a non-empty `"foo"` → `""` (catches tests that don't pin the returned string) |
| `EMPTY->NONEMPTY` | an empty `""` → a sentinel (catches an unchecked empty-string case) |

## Reading the result

- **Mutation score** = killed / (killed + survived). Higher is better; 100%
  means every single-operator change that *compiled* was caught by some test.
- **Survivors** are your to-do list: each one is reported as
  `file:line  MUTATION` so you can jump straight to the unguarded code. Either
  add a test that distinguishes it, or convince yourself it's an *equivalent
  mutant* (see caveats).
- **No-compile** mutants (the mutation produced invalid code) are reported as
  `(N excluded — did not compile)` and left out of the denominator — they were
  never really tested, so they neither help nor hurt the score. With the core
  operator set they're rare (most operator swaps stay valid), but the category
  keeps the score honest when they happen.

## Honest limitations

This is a Tier-1, text-based tool. Know what it does and doesn't do:

- **Text, not AST.** Operators are matched as padded tokens, so `++`, `+=`,
  and operators that abut other characters are skipped. The tool *is*
  string-boundary aware — a padded operator inside a `"..."` literal won't be
  mutated as code, and string-literal mutators only touch real literals. The
  remaining blind spot is **comments**: a `"..."` or a padded operator written
  in a `//` comment is still treated as source, so it can produce a harmless
  false mutant (it changes nothing the suite sees → survives). Keep
  operator/quote characters out of comments in a SUT you mutate, or expect a
  stray survivor.
- **Equivalent mutants.** Some changes don't alter behaviour (e.g. `<` → `<=`
  on a boundary your code never reaches). They "survive" without being real
  gaps. This is inherent to mutation testing, not a bug here.
- **Slow.** Every mutant pays a cache-clear + `ae check` + `ae build` + run —
  and the cache-clear is mandatory (an imported-module edit doesn't invalidate
  the cache, so you'd otherwise test a stale build). Three compiler invocations
  per mutant, no warm-cache reuse — point it at a focused SUT, not your whole
  codebase.
- **Crash safety.** The driver restores the original SUT at the end of the run
  (the regression fixtures verify byte-identical restore). But it mutates the
  real file in place, so if the driver is killed mid-run (Ctrl-C, OOM), the
  SUT is left mutated — recover with `git checkout <sut.ae>`. Run it on a
  clean working tree.
- **POSIX-only.** The oracle shells out through `/bin/sh` and
  `os.run_pipe_drain_and_wait`; Windows is not supported.

## The upgrade path (why this lives in the aether tree)

Every limitation above is a compiler-adjacency problem: text-based mutation
can produce mutants a parser would never emit; every mutant pays full
rebuilds; there's no equivalent-mutant detection; and the #953 grep-workaround
exists only because the tool used to sit outside the compiler. Precise,
false-mutant-free mutation needs **AST-level edits** — operator sites from the
real parser, positions from `ae inspect`-grade data, perhaps an `ae mutate`
subcommand. Adopting the Tier-1 tool into the tree puts that upgrade path
where the compiler is.

Regression coverage: `tests/integration/mutation_testing/` (a deterministic
operator fixture pinning `1/2 … 50%` with a MUL→DIV survivor and byte-identical
SUT restore, plus a string-literal fixture covering the boundary-awareness
skip).
