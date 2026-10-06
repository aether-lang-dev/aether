#!/bin/sh
# `x op= y` on a variable a closure captures by reference.
#
# Such a variable lives in a shared cell, and its C name is the cell's
# pointer (`int* x`) both in the closure body and in the function that
# declares it. A plain assignment writes `*x = ...`; the compound form was
# emitted as `x += 1`, which is pointer arithmetic on the cell: the value
# never changed, and the next `*x` read memory past the cell. No diagnostic,
# just a wrong number (the closure printed 0 where 30 was due).
#
# The target of `x op= y` is held in the node's value, not in an identifier
# child, and two analyses missed it for that reason: closure capture (a
# closure whose only use of a variable was `+=` did not capture it, "'count'
# undeclared"; one that captured it otherwise took it by value) and the
# try-body write scan that makes such a variable volatile.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] closure_capture_compound_assign: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
main() {
    // In the closure body.
    x = 2
    g = || {
        x += 1
        x = x * 10
        println("inner ${x}")
    }
    call(g)
    println("outer ${x}")

    // In the declaring function, after the capture.
    n = 1
    h = || { n = n + 100 }
    n += 5
    call(h)
    n *= 2
    println("decl ${n}")

    // A float cell, every compound operator.
    f = 1.5
    k = || {
        f *= 4.0
        f -= 1.0
        f /= 2.0
    }
    call(k)
    println("float ${f}")

    // A counter bumped across calls.
    count = 0
    tick = || { count += 1 }
    i = 0
    while i < 5 {
        call(tick)
        i = i + 1
    }
    println("count ${count}")

    // In a try body: `+=` marks its target volatile like `=` does, so the
    // catch reads the value the body left, not a register's stale copy.
    t = 0
    try {
        t += 5
        panic("boom")
    } catch e {
        println("try ${t}")
    }
}
AE

want="inner 30
outer 30
decl 212
float 2.5
count 5
try 5"
got="$("$AE" run "$tmp/main.ae" 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] closure_capture_compound_assign: wrong values"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi
# Whether a non-volatile `t` reads stale after the unwind depends on the
# optimiser's register choice, so the value check above cannot always see a
# missing mark. The declaration can: `t` is written by `+=` in a try body.
if ! "$ROOT/build/aetherc" "$tmp/main.ae" "$tmp/out.c" >/dev/null 2>&1; then
    echo "  [FAIL] closure_capture_compound_assign: aetherc failed"
    exit 1
fi
if ! grep -q 'volatile int t = 0;' "$tmp/out.c"; then
    echo "  [FAIL] closure_capture_compound_assign: 't', written by '+=' in a try body, is not declared volatile"
    exit 1
fi
echo "  [PASS] closure_capture_compound_assign: += -= *= /= write through a captured cell, capture it, and mark it volatile in a try body"
