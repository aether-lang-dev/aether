# std.spec

A BDD test framework: `describe`, `it`, hooks, assertions and structured
reports.

Aether's own `std` modules test themselves with it — every `std/<mod>/test_*.ae`
is a `std.spec` suite — so the framework is exercised by the same runs that
exercise the library.

```aether
import std.spec

main() {
    fw = spec.init()

    spec.describe(fw, "arithmetic") {
        spec.it("adds") callback {
            spec.assert_eq(2 + 2, 4, "2+2")
        }
        spec.it("multiplies") callback {
            spec.assert_eq(3 * 3, 9, "3*3")
        }
    }

    spec.run_summary(fw)
}
```

It prints a green tick per passing case and a summary:

```
arithmetic
  ✓ adds
  ✓ multiplies

  2 passing
```

(The block above is compiled but not run in CI: `std.spec` colours its
output with ANSI escapes, which do not belong in an asserted `output`
block.)

`run_summary` exits non-zero when anything failed, so a suite works as a test
binary without extra wiring.

## Assertions

`assert_eq` takes `int`. **Use `assert_eq_long` for anything wider than 32
bits** — a `mem.get_long`, a u64 accessor, an IEEE 754 bit pattern. Narrowing
at the call boundary is not merely a wrong comparison: it has aborted the
process on Windows while passing on Linux, where both truncated halves happened
to agree.

For the `(value, err)` convention that runs through `std`, the failure-path
matchers print the error rather than discarding it:

```aether,fragment
value, err = thing.parse(input)
spec.assert_ok(err, "parse should accept good input")
spec.assert_err(err, "parse should reject bad input")
spec.assert_err_contains(err, "unexpected token", "and should say why")
```

`assert_ok` is worth reaching for even when a call "obviously" succeeds: it is
the assertion that catches the day it stops.

## Hooks

`before_each()` takes **no argument** — the context is injected by the
trailing-block mechanism. Passing `fw` explicitly registers the hook on the
framework root instead of the enclosing `describe`, which silently means it
never runs between the cases you meant it for.

## Skips

`it_when`, `skip_it` and `skip_all_if` mark a case as not-run rather than
passing it silently. Skips are always reported when non-zero, so a run that
skipped everything cannot be mistaken for a run that passed everything.

## Known limitation

Sandbox grants are ignored inside a `describe` block (#1704), so `std.spec`
cannot currently test sandboxed behaviour.

## Exports

`init`, `describe`, `it`, `it_within`, `it_when`, `skip_it`, `skip_all_if`,
`before_each`, `after_each`, `fail`, `assert_true`, `assert_false`,
`assert_eq`, `assert_eq_long`, `assert_str_eq`, `assert_str_eq_diff`,
`assert_not_eq`, `assert_gt`, `assert_contains`, `assert_null`,
`assert_not_null`, `assert_ok`, `assert_err`, `assert_err_contains`,
`expect_elapsed_under`, the fluent `expect_int` / `expect_str` facade, and
`run_summary`.
