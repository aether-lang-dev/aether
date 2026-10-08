#!/bin/sh
# Calling a closure literal bound without a type yields the type its body
# returns (#2460).
#
# A closure literal's type is the erased `fn`, so `call(f, ...)` on it was
# typed int: `r = call(sg, "b")` printed the string's address as a number
# and a float result printed 0, with an "assumed int" warning. The checker
# now types the call from the literal the variable holds, through an alias,
# a capture, a closure that returns a closure, and a re-bind to a closure
# with the same result. This test checks the output and that no warning is
# printed; tests/regression/test_closure_literal_call_result.ae covers
# ownership of string results.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] closure_literal_call_result: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
import std.string

struct Pt {
    x: int
    name: string
}

main() {
    // The issue's program.
    sg = |t: string| -> t
    r2 = call(sg, "b")
    println(r2)
    fl = |a: float| -> a * 2.0
    r5 = call(fl, 1.25)
    println("${r5}")

    // Results straight into println and interpolation.
    pos = |v: int| -> v > 0
    println(call(pos, -1))
    println("${call(sg, "direct")} ${call(fl, 0.25)}")

    // A struct result.
    mk = |a: int| -> Pt { x: a, name: "p" }
    println("${call(mk, 7).x} ${call(mk, 8).name}")

    // A closure that returns a closure, two levels.
    adder = |k: string| {
        return |s: string| -> string.concat(s, k)
    }
    bang = call(adder, "!")
    println(call(bang, "hey"))
    maker = || {
        return |n: int| {
            return |m: float| -> m + n
        }
    }
    step = call(maker)
    plus = call(step, 2)
    println("${call(plus, 0.5)}")

    // A captured closure variable called inside another closure, and an
    // alias of one.
    twice = |v: float| -> call(fl, call(fl, v))
    println("${call(twice, 1.5)}")
    fl2 = fl
    println("${call(fl2, 4.0)}")

    // Re-bound to a closure with the same result, in a branch and a loop.
    h = |a: int| -> "s${a}"
    if r2 == "b" {
        h = |a: int| -> "t${a}"
    }
    println(call(h, 2))
    k = |a: int| -> a + 1.5
    i = 0
    while i < 2 {
        k = |a: int| -> a * 2.0
        i = i + 1
    }
    println("${call(k, 3)}")

    // The direct-call spelling.
    d = fl(3.0)
    println("${d}")
    println(sg("e"))
}
AE

want="b
2.5
false
direct 0.5
7 p
hey!
2.5
6
8
t2
6
6
e"
got="$("$AE" run "$tmp/main.ae" 2>&1 | tr -d '\r')"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] closure_literal_call_result: wrong output"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] closure_literal_call_result: calls of closure literals carry their string, float, bool, struct and closure results"
