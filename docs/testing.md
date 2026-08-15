# Testing with `std.spec` and `ae test`

Aether ships a BDD-style test framework in the standard library
(`std.spec`) and a discovery runner in the toolchain (`ae test`). Together
they cover the common shape: describe a suite, write `it` cases, assert
with a flat or fluent API, and run every `test_*.ae` / `*_test.ae` file
with one command.

`std.spec` is the pure, dependency-light core of the standalone
[aeocha](https://github.com/aether-lang-dev/aeocha) framework, folded into
the stdlib. The process- and HTTP-shaped integration matchers and the
mutation-testing facility remain in aeocha; `std.spec` is
`describe`/`it`/hooks, the assertion families, and structured reporting.

## A first test

`std.spec` is a normal stdlib module — no install step, no `--lib`:

```aether
import std.spec

main() {
    fw = spec.init()

    spec.describe(fw, "calculator") {
        spec.it("adds two numbers") callback {
            spec.assert_eq(2 + 3, 5, "2 + 3")
        }
        spec.it("prefers the fluent style") callback {
            spec.expect_int(abs(-7)).to_equal(7)
        }
    }

    spec.run_summary(fw)
}
```

Run it directly:

```bash
ae run calc_test.ae
```

or let the runner discover it:

```bash
ae test              # every test_*.ae / *_test.ae under tests/
ae test calc_test.ae # a single file
ae test path/to/dir  # a directory (its tests/ subdir if present, else itself)
```

Each test file is a standalone program with its own `main()`. `ae test`
compiles and runs each one and reads its **process exit code**: `0` means
all assertions passed, non-zero means at least one failed.
`spec.run_summary(fw)` prints the totals and calls `exit(1)` when any
failure was recorded, so the exit-code contract is automatic.

## Suites, context, and hooks

`init()` returns a framework context and makes it the process's *ambient*
framework. The top-level `describe` and `run_summary` take that `fw`;
everything nested inside a suite omits it, because Aether injects the
suite context (`_ctx`) into the trailing block.

```aether
spec.describe(fw, "outer") {
    spec.describe("inner") {           // no fw — injected
        spec.before_each() callback { reset() }
        spec.after_each() callback { close_resources() }
        spec.it("does something") callback { /* ... */ }
    }
}
```

`before_each` runs outermost-to-innermost, `after_each`
innermost-to-outermost, and both are inherited by descendant suites. One
framework is supported per process — a second `init()` rebinds the
ambient context.

Because a passing run *returns* (rather than exiting), a test that starts
a long-running actor or server should stop it or end `main()` with an
explicit `exit(0)`.

## Assertions

Failures are **soft**: a failed assertion records the failure, prints it,
and returns, so several checks in one `it` all report. Any recorded
failure makes that `it` red.

### Flat assertions

```aether
spec.assert_eq(count, 4, "four jobs")          // int equality
spec.assert_not_eq(a, b, "distinct")
spec.assert_gt(size, 0, "non-empty")
spec.assert_true(ok, "the thing worked")
spec.assert_false(err, "no error")
spec.assert_str_eq(name, "Ada", "user name")   // exact string equality
spec.assert_str_eq_diff(got, want, "payload")  // caret under first diff
spec.assert_contains(body, "world", "greeting")
spec.assert_null(ptr, "was freed")
spec.assert_not_null(user, "was loaded")
```

`assert_str_eq_diff` aligns the two values and points a caret at the first
differing byte — reach for it when a plain "expected X got Y" would bury
the difference in a long string.

### Fluent assertions

A subject-first, chainable facade over the same soft-failure machinery:

```aether
spec.expect_int(count).to_be_gt(0).to_equal(4)
spec.expect_int(exit_code).not_().to_equal(1)
spec.expect_str(name).to_start_with("ae").to_contain("the")
```

Integer and string starters are separate because Aether has no
receiver-type overloading, so string equality is `to_equal_str` while
integer equality is `to_equal`. `not_()` negates the next matchers on an
integer subject (start a fresh `expect_int` to return to positive
matching). `satisfies` / `satisfies_str` drop an arbitrary
`fn(value) -> 1/0` predicate into the chain.

Available matchers: `to_equal`, `to_be_gt`, `to_be_lt`, `to_be_truthy`,
`to_be_falsy` (int); `to_equal_str`, `to_contain`, `to_start_with`
(string); `satisfies`, `satisfies_str` (both).

### Extending the fluent chain

`IntSubject` and `StrSubject` are exported. A free function whose first
parameter is one of those types participates in Aether's UFCS method
syntax — no base class, no registration:

```aether
to_be_even(s: spec.IntSubject, msg: string) -> spec.IntSubject {
    if s.value % 2 != 0 { spec.fail("${msg} — ${s.value} is not even") }
    return s
}

spec.expect_int(count).to_be_gt(0).to_be_even("even count")
```

Return the subject so callers can keep chaining. A flat custom matcher is
even simpler — any function that calls `spec.fail(msg)`:

```aether
expect_prime(n: int, msg: string) {
    if !is_prime(n) { spec.fail("${msg} — ${n} is not prime") }
}
```

### Collection matchers

Operate on a `std.list` of strings — the common shape for captured lines,
names, or ids:

```aether
spec.expect_list_size(xs, 3, "three rows")
spec.expect_list_empty(ys, "nothing pending")
spec.expect_list_has_str(xs, "alpha", "has alpha")
spec.expect_list_contains_all(xs, wanted, "all present")   // order-independent
spec.expect_list_every(xs, is_nonempty, "no blanks")       // empty passes
```

### Timing budgets

`it_within` runs a normal test and *also* fails it when the body meets or
exceeds a `Duration` budget:

```aether
spec.it_within("responds promptly", 50ms) callback {
    call_service()
}
```

`expect_elapsed_under` does the same comparison for a monotonic-ns span
the caller measures itself:

```aether
t0 = os.now_monotonic_ns()
do_work()
spec.expect_elapsed_under(os.now_monotonic_ns() - t0, 50ms, "fast path")
```

Use monotonic-clock deltas (not wall-clock) — only the delta is
meaningful and a wall jump would corrupt it.

## Structured reporting

By default `ae test` prints human progress and a `PASS`/`FAIL` line per
file. Machine-readable reporting is **opt-in** behind `--format`:

```bash
ae test --format=tap          # one aggregated TAP version 13 stream
ae test --format=aeocha-v1    # one aeocha v1 block per test file
```

In a report mode the per-file progress lines and the human summary are
suppressed, so stdout carries only the machine stream; the process exit
code still reflects pass/fail.

### TAP

`--format=tap` emits a single [TAP version 13](https://testanything.org/)
document with the test points from every file renumbered into one
sequence and the plan at the end. A failing case carries its captured
message as a YAML diagnostic block:

```
TAP version 13
ok 1 - adds two numbers
ok 2 - prefers the fluent style
not ok 3 - handles overflow
  ---
  message: "expected 5, got 6"
  ...
1..3
```

A test file that emits no structured report (a hand-rolled test that
doesn't use `std.spec`, or one that crashes before `run_summary`)
contributes a single point derived from its exit code.

### aeocha v1

`--format=aeocha-v1` emits the aeocha v1 report format — a `version=1`
key/value header, a `---` separator, then tab-packed `PASS`/`FAIL` rows
(status, index, quoted name, per-row nanosecond duration, and the failure
message for a FAIL). `ae test` emits one block per file, each preceded by
a `# <path>` comment line, so multiple blocks are separable (split on
`version=1`).

### How it works (and running it yourself)

Under a `--format` flag, `ae test` hands each child two environment
variables — `AE_SPEC_FORMAT` (the format name) and `AE_SPEC_REPORT` (a
path) — and `spec.run_summary` writes the report there. You can drive that
directly without the runner:

```bash
AE_SPEC_FORMAT=tap AE_SPEC_REPORT=/tmp/out.tap ae run calc_test.ae
cat /tmp/out.tap
```

With neither variable set, `run_summary` writes no report and the human
`✓`/`✗` output is the only output — the default.

## Conventions

- Name test files `test_*.ae` or `*_test.ae` so `ae test` discovers them
  (pytest / Go convention).
- Keep test source canonically formatted; CI enforces it
  (`ae fmt tests`).
- Assertions are soft — prefer several small `expect_`/`assert_` checks
  over one compound condition, so a failure names exactly what broke.

## See also

- [Standard Library Reference](stdlib-reference.md) — the full stdlib surface.
- [Closures and Builder DSL](closures-and-builder-dsl.md) — the
  trailing-block / `_ctx` injection mechanism `describe`/`it` are built on.
- [aeocha](https://github.com/aether-lang-dev/aeocha) — the upstream
  framework, including the process/HTTP integration matchers and mutation
  testing that live outside the stdlib.

## Domain matcher arms: `std.os.testing` and `std.http.client.httptest`

The framework core (`std.spec`) is deliberately dependency-light. Two
sibling modules carry the integration-shape matchers that consume other
std surfaces, reporting through the same ambient framework cell — so
they compose with `describe`/`it` and count into the same summary:

- **`import std.os.testing`** — process matchers over the
  `(stdout, exit, stderr)` triple from `os.run_capture`:
  `testing.expect_exit`, `testing.expect_no_spawn_error`,
  `testing.expect_stdout_contains` / `_line_count` / `_line_field` /
  `_line_after` / `_matches` / `_matches_regex`, and the stderr pair
  `testing.expect_stderr_contains` / `_empty`. They absorb the
  bash-shaped "spawn / capture / awk-and-compare" idiom into one call.

- **`import std.http.client.httptest`** (named per Go's
  `net/http/httptest`; the namespace tail must differ from
  `std.os.testing`'s) — response-handle matchers
  (`httptest.expect_http_status` / `_no_error` / `_body_eq` /
  `_body_contains` / `_header` / `_body_json_field`), one-call
  GET/POST conveniences (`httptest.expect_http_get_status`, …), and the
  FluentSelenium-style one-shot retry budgets `httptest.within(2s)` /
  `httptest.without(2s)` that the convenience family honours.

Both are ported verbatim from the standalone aeocha framework, which
retains its own copies (and the mutation-testing facility) for
standalone use. See `tests/integration/std_testing_arms/probe.ae` for
an end-to-end example driving both arms against real subprocesses and
an in-process fixture server.

## Test report format (aeocha-v1) — a stable, versioned contract

`std.spec`'s `run_summary` writes a machine-readable report to the path
in `AE_SPEC_REPORT` when `AE_SPEC_FORMAT=aeocha` (or `aeocha-v1`) is
set; `ae test --format=aeocha-v1` arranges both per child. Downstream
tools (aeb's `aether.driver_test` reporting among them) parse this file,
so its shape is a **contract, not an implementation detail**:

```
version=1
total=<N>
passed=<N>
failed=<N>
errored=<N>
duration_ms=<N>
duration_ns=<N>
---
<STATUS>\t<index>\t"<name>"\t<duration_ns>[\t<message>]
...
```

The rules a consumer may rely on, and an editor must preserve:

1. The header is `key=value` lines, one per line, starting with
   `version=`. Within `version=1` the keys above keep their names and
   meanings. **New keys may be added** (consumers must ignore unknown
   keys); existing keys are never renamed, repurposed, or removed.
2. The header ends at the first `---` line (`\n---\n`); rows follow.
3. Each row is tab-separated: STATUS (`PASS`/`FAIL`), a 1-based index,
   the double-quoted test name, a duration (nanoseconds today — treat
   the unit as unspecified within v1 and do not compute from it), and —
   only when present — a trailing message field.
4. **Any change that breaks rules 1–3 bumps the leading `version=`**
   so a pinned consumer detects v2 instead of misparsing v1.

`_format_aeocha_v1` in `std/spec/module.ae` is the single producer;
it carries a pointer back to this section.

## `contrib.aeocha` is retired

The aeocha framework was absorbed into the stdlib: `import std.spec`
(core + fluent + collections + timing), `import std.os.testing`
(process matchers), `import std.http.client.httptest` (HTTP matchers,
`within`/`without`/`eventually`). No std or contrib module references
`contrib.aeocha`, and nothing in a test's closure requires the aeocha
repo or its old IPC-pipe report convention — the env-file transport
above replaces it. Downstreams still importing `contrib.aeocha` from
pre-spinout snapshots should migrate to the modules above; the
standalone aeocha repo remains only as a thin compatibility facade
plus its aeb IPC reporter and mutation-testing tool.
