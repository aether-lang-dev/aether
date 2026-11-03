#!/bin/sh
# Regression (#3524): an integer division or remainder by zero in a constant
# expression is a compile error that names the expression. `const B = 10 / 0`
# reached the C compiler, which refused the static's initializer or folded
# it to anything; `x = 10 / (5 - 5)` compiled to a division the program
# could only panic on. The compiler computes such an expression, literals,
# `const`s and the operators over them in their own width (`65536 * 65536`
# is 0 in an `int`), and refuses it there. A divisor known only at run time
# still panics with division_by_zero, and a local or parameter named like a
# constant is that binding, not the constant.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
[ -x "$AETHERC" ] || AETHERC="$ROOT/build/aetherc.exe"
AE="$ROOT/build/ae"
[ -x "$AE" ] || AE="$ROOT/build/ae.exe"
NAME=constant_division_by_zero

if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] $NAME: $AETHERC not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT
export AETHER_HOME="$ROOT"
fail=0

# expect <name> <message fragment> <program>
expect() {
    printf '%s\n' "$3" > "$TMPDIR/$1.ae"
    if "$AETHERC" --check "$TMPDIR/$1.ae" > "$TMPDIR/$1.out" 2>&1; then
        echo "  [FAIL] $NAME: $1 compiled"
        fail=1
    elif ! grep -qF -- "$2" "$TMPDIR/$1.out"; then
        echo "  [FAIL] $NAME: $1 did not say: $2"
        sed 's/^/    /' "$TMPDIR/$1.out" | head -6
        fail=1
    fi
}

expect const_init "division by zero in the constant expression \`10 / 0\`" \
'const B = 10 / 0
main() { println("${B}") }'

expect named_const "remainder by zero in the constant expression \`7 % Z\`" \
'const Z = 0
const C = 7 % Z
main() { println("${C}") }'

expect in_function "division by zero in the constant expression \`10 / (5 - 5)\`" \
'main() {
    x = 10 / (5 - 5)
    println("${x}")
}'

expect long_divisor "division by zero in the constant expression \`5000000000 / (N - 4)\`" \
'const N = 4
main() {
    long y = 5000000000 / (N - 4)
    println("${y}")
}'

expect wraps_to_zero "division by zero in the constant expression \`10 / (65536 * 65536)\`" \
'main() {
    x = 10 / (65536 * 65536)
    println("${x}")
}'

# What stays a value: a constant divisor that is not 0 (a shift count is
# masked, so 1 << 32 is 1), MIN / -1, a name bound like a constant, and a
# divisor known at run time, which panics.
cat > "$TMPDIR/accept.ae" <<'AE'
const ZERO = 0
const W = 10 / 2
const WRAP = (-2147483647 - 1) / -1

by(ZERO: int) -> int { return 10 / ZERO }

main() {
    ZERO = 3
    a = 10 / ZERO
    b = 10 / (1 << 32)
    z = 0
    try {
        println("${10 / z}")
    } catch e {
        println("caught ${e}")
    }
    println("${W} ${WRAP} ${a} ${b} ${by(5)}")
}
AE
if ! "$AE" build "$TMPDIR/accept.ae" -o "$TMPDIR/accept" > "$TMPDIR/accept.log" 2>&1; then
    echo "  [FAIL] $NAME: a program with no constant division by zero was refused"
    sed 's/^/    /' "$TMPDIR/accept.log" | head -8
    fail=1
else
    out=$("$TMPDIR/accept" 2>&1 | tr -d '\r' | sed 's/ at .*//')
    want="caught division_by_zero: division by zero
5 -2147483648 3 10 2"
    if [ "$out" != "$want" ]; then
        echo "  [FAIL] $NAME: wrong values"
        printf '%s\n' "$out" | sed 's/^/    /'
        fail=1
    fi
fi

[ "$fail" = 0 ] && echo "  [PASS] $NAME: a constant division by zero is a compile error naming it; the rest compile"
exit $fail
