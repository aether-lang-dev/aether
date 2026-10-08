#!/bin/sh
# A const whose initializer is an operator expression is typed from it, also
# when another module reads it (#2475).
#
# Registration typed a const from a bare literal only (#1857); `1 << 30` stayed
# unknown until the second pass reached the declaration. An imported const is
# merged at the end of the program and a same-file const can follow its users,
# so `mark = n.flag_index & flags.MOVED` bound an untyped local: codegen warned
# "unresolved type in codegen, defaulting to int", and for a 64-bit const the
# int was a silent truncation (`v | HIGH` with HIGH = 1 << 34 printed v).
#
# Covers << >> & | ^ ~, a const built from other consts, a const in a second
# module built from the first module's consts, a same-file const declared after
# its use, and a selective import. Asserts the values and that the warning is
# gone.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] imported_const_expr_type: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"
cd "$tmp" || exit 1

fail() {
    echo "  [FAIL] imported_const_expr_type: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2" | head -20
    exit 1
}

mkdir -p flags bits
cat > flags/module.ae <<'AE'
exports ( MOVED, LOW, BOTH, MASK, NOT_MOVED, HIGH, DOWN, XORED, Node )

const MOVED = 1 << 30
const LOW = 0x0F
const BOTH = MOVED | LOW
const MASK = ~LOW
const NOT_MOVED = MASK & ~MOVED
// 0x100000000 is a 64-bit literal, so HIGH is too.
const HIGH = 0x100000000 << 2
const DOWN = MOVED >> 4
const XORED = BOTH ^ LOW

struct Node {
    flag_index: int
}
AE

cat > bits/module.ae <<'AE'
import flags

exports ( WIDE, tagged )

const WIDE = flags.HIGH | flags.MOVED

tagged(v: int) -> int {
    t = v & flags.MOVED
    return t
}
AE

cat > main.ae <<'AE'
import flags
import bits

check(n: Node) -> int {
    mark = n.flag_index & flags.MOVED
    return mark
}

probe(v: int) {
    a = v | flags.LOW
    b = v ^ flags.XORED
    c = v & flags.NOT_MOVED
    d = flags.DOWN >> 2
    e = ~flags.BOTH
    w = flags.HIGH | v
    x = bits.WIDE & flags.HIGH
    y = LATE << 1
    println("${a} ${b} ${c} ${d} ${e} ${w} ${x} ${y} ${bits.tagged(v)}")
}

main() {
    n = Node { flag_index: 1 << 30 }
    println("${check(n)}")
    probe(1073741825)
}

const LATE = flags.MOVED >> 8
AE

cat > sel.ae <<'AE'
import flags (HIGH, MOVED)

wide(v: int) -> long {
    w = v | HIGH
    m = v & MOVED
    return w + m
}

main() {
    println("${wide(1)}")
}
AE

# run <file> <want>
run() {
    "$AE" run "$1" > "$1.log" 2>&1 || fail "$1 did not run" "$1.log"
    tr -d '\r' < "$1.log" > "$1.txt"
    if grep -q "unresolved type in codegen" "$1.txt"; then
        fail "$1: still warns 'unresolved type in codegen'" "$1.txt"
    fi
    got="$(grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$" "$1.txt")"
    if [ "$got" != "$2" ]; then
        printf '  [FAIL] imported_const_expr_type: %s\ngot:\n%s\nwant:\n%s\n' \
            "$1" "$got" "$2" | sed '2,$s/^/        /'
        exit 1
    fi
}

run main.ae "1073741824
1073741839 1 0 16777216 -1073741840 18253611009 17179869184 8388608 1073741824"
run sel.ae "17179869185"

echo "  [PASS] imported_const_expr_type: operator-valued consts keep their type across modules"
