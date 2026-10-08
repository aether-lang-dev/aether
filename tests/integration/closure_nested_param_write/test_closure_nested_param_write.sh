#!/bin/sh
# A closure nested in another closure can write the outer closure's
# parameter (#2463).
#
# Capture promotion recorded the parameter as promoted in the outer
# closure's scope, but the outer closure kept it a plain value, so the
# nested closure's environment was handed an `int` where it holds an
# `int*` cell: the C compiler stopped at "passing argument 1 of
# '_aether_cell_retain' makes pointer from integer". The outer closure now
# takes such a parameter as `_param_<name>` and keeps it in a shared cell,
# as a function does for a parameter a closure writes, so each write is
# seen by the closure that owns the parameter.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] closure_nested_param_write: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
import std.string

struct Pt {
    x: int
    y: int
}

main() {
    // The issue's shape, the parameter read again after the nested call.
    outer = |p: int| {
        inner = || { p = p + 1 }
        call(inner)
        call(inner)
        return p * 10
    }
    println("${call(outer, 10)}")

    // Two levels deep: the middle closure forwards the cell.
    deep = |q: int| {
        mid = || {
            leaf = || { q = q * 2 }
            call(leaf)
            call(leaf)
        }
        call(mid)
        return q
    }
    println("${call(deep, 3)}")

    // A string parameter, written in a loop and by the owner itself.
    tag = |s: string, n: int| {
        s = "${s}-"
        add = || { s = "${s}!" }
        i = 0
        while i < n {
            call(add)
            i = i + 1
        }
        return s
    }
    println(call(tag, "ab", 3))

    // A string parameter two levels deep.
    sdeep = |s: string| {
        mid = || {
            leaf = || { s = "${s}x" }
            call(leaf)
        }
        call(mid)
        call(mid)
        return s
    }
    println(call(sdeep, "w"))

    // `+=` and `++` from the nested closures.
    cnt = |k: int| {
        a = || { k += 5 }
        b = || { k++ }
        call(a)
        call(b)
        return k
    }
    println("${call(cnt, 0)}")

    // A struct parameter written through a field.
    mv = |pt: Pt| {
        step = || { pt.x = pt.x + 1 }
        call(step)
        call(step)
        return pt.x + pt.y
    }
    println("${call(mv, Pt { x: 1, y: 100 })}")

    // Float and bool parameters.
    fl = |f: float, on: bool| {
        h = || {
            f = f * 2.0
            on = !on
        }
        call(h)
        if on {
            return f
        }
        return 0.0 - f
    }
    println("${call(fl, 1.25, false)}")

    // Each call of the outer closure gets a fresh cell.
    j = 0
    total = 0
    while j < 4 {
        total = total + call(outer, j)
        j = j + 1
    }
    println("${total}")
}
AE

want="120
12
ab-!!!
wxx
6
103
2.5
140"
got="$("$AE" run "$tmp/main.ae" 2>&1 | tr -d '\r')"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] closure_nested_param_write: wrong output"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] closure_nested_param_write: nested closures write an outer closure's int, string, struct, float and bool parameters"
