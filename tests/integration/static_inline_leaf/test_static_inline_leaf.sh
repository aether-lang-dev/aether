#!/bin/sh
# Small leaf functions from imported modules are emitted `static inline`
# (#2123).
#
# Every Aether function reached C as plain `static` (for imported and
# file-local functions) and gcc's -O2 budget for a function not declared
# inline left a physics engine's 3x3 products and quaternion rotations out
# of line — 1.6x on a joint-grid solve against the same functions marked
# inline by hand. A function with internal linkage whose body is small,
# has no loop, no match, no closure and does not call itself is now
# `static inline`, prototype and definition alike; everything else is
# unchanged, and a program's own top-level functions keep external linkage
# (C99 `inline` on those would change what the translation unit must
# provide).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] static_inline_leaf: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

mkdir -p "$tmp/mathx"
cat > "$tmp/mathx/module.ae" <<'AE'
exports(dot, mul, len_sq, sum_to, fact, pick)
struct V3 { x: float, y: float, z: float }
dot(a: V3, b: V3) -> float { return a.x * b.x + a.y * b.y + a.z * b.z }
mul(a: V3, k: float) -> V3 { return V3 { x: a.x * k, y: a.y * k, z: a.z * k } }
len_sq(a: V3) -> float { return dot(a, a) }
sum_to(n: int) -> int {
    s = 0
    i = 0
    while i <= n { s = s + i; i = i + 1 }
    return s
}
fact(n: int) -> int { if n <= 1 { return 1 } return n * fact(n - 1) }
pick(n: int) -> int {
    match n {
        0 -> { return 10 }
        _ -> { return 20 }
    }
}
AE
cat > "$tmp/main.ae" <<'AE'
import mathx
twice(x: int) -> int { return x * 2 }
main() {
    v = mathx.V3 { x: 1.0, y: 2.0, z: 3.0 }
    println("${mathx.dot(v, v)} ${mathx.len_sq(mathx.mul(v, 2.0))} ${mathx.sum_to(4)} ${mathx.fact(5)} ${mathx.pick(0)} ${twice(4)}")
}
AE
got="$(cd "$tmp" && AETHER_LIB_DIR="$tmp" AETHER_HOME="$ROOT" "$AE" run main.ae 2>&1 | tail -1)"
if [ "$got" != "14 56 10 120 10 8" ]; then
    echo "  [FAIL] static_inline_leaf: program output differs (got '$got')"
    fail=1
fi

(cd "$tmp" && AETHER_LIB_DIR="$tmp" AETHER_HOME="$ROOT" "$AETHERC" main.ae out.c >/dev/null 2>&1)
# leaf maths: inline, prototype and definition
for fn in mathx_dot mathx_mul mathx_len_sq; do
    n=$(grep -c "^static inline AETHER_MAYBE_UNUSED [A-Za-z0-9_]* $fn(" "$tmp/out.c")
    if [ "$n" != "2" ]; then
        echo "  [FAIL] static_inline_leaf: $fn should be 'static inline' in prototype and definition (found $n)"
        fail=1
    fi
done
# a loop, a recursion, a match: plain static
for fn in mathx_sum_to mathx_fact mathx_pick; do
    if grep -q "^static inline AETHER_MAYBE_UNUSED [A-Za-z0-9_]* $fn(" "$tmp/out.c"; then
        echo "  [FAIL] static_inline_leaf: $fn must not be inline"
        fail=1
    fi
    if ! grep -q "^static AETHER_MAYBE_UNUSED [A-Za-z0-9_]* $fn(" "$tmp/out.c"; then
        echo "  [FAIL] static_inline_leaf: $fn should still be static"
        fail=1
    fi
done
# the program's own function keeps external linkage
if grep -q "static.*twice(" "$tmp/out.c"; then
    echo "  [FAIL] static_inline_leaf: a top-level program function became static/inline"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] static_inline_leaf: small leaf module functions are static inline; loops, recursion, match and program functions are not"
fi
exit $fail
