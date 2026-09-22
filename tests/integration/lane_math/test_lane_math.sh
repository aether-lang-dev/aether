#!/bin/sh
# #2158: a lane square root and absolute value.
#
# A solver's friction and rolling clamps normalise an impulse by the square
# root of its squared length, once per lane bundle. Without a lane sqrt the
# port writes four `lane4` extracts, four scalar `sqrtf` calls and a rebuild,
# where the hardware has had a one-instruction lane square root since SSE2.
# That was the one place where Aether's lanes emitted more than the C they
# replaced (aether-lang-dev/aephysics#46).
#
# Both operations are EXACT: an IEEE square root is correctly rounded, and
# clearing the sign bit is not an approximation. So this asserts equality
# with the scalar form rather than a tolerance — a lane version that agreed
# only approximately would be the wrong thing to ship.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] lane_math: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
import std.lanes
import std.math

main() {
    // Exact values first, so the printed form is unambiguous.
    r = lanes.sqrt4(lanes.f32x4(9.0, 16.0, 25.0, 2.25))
    println("sqrt4 ${lanes.lane4(r, 0)} ${lanes.lane4(r, 1)} ${lanes.lane4(r, 2)} ${lanes.lane4(r, 3)}")

    // -0.0 must come back as +0.0 and contribute nothing to the sum.
    a = lanes.abs4(lanes.f32x4(-3.5, 3.5, -0.0, -7.25))
    println("abs4 ${lanes.lane4(a, 0)} ${lanes.lane4(a, 2)} ${lanes.sum4(a)}")

    d = lanes.sqrt2(lanes.f64x2(81.0, 6.25))
    println("sqrt2 ${lanes.lane2(d, 0)} ${lanes.lane2(d, 1)}")
    e = lanes.abs2(lanes.f64x2(-4.5, 4.5))
    println("abs2 ${lanes.lane2(e, 0)} ${lanes.lane2(e, 1)}")

    // A value with no exact square root: the lane result must equal the
    // scalar one BIT for BIT, because both are the correctly-rounded IEEE
    // square root of the same input. An approximation would not.
    x = 2.0
    lane_sqrt = lanes.lane4(lanes.sqrt4(lanes.splat4(x as f32)), 0)
    scalar = math.sqrt(x) as f32
    println("exact ${lane_sqrt == scalar}")

    // A negative lane yields NaN, as sqrt does; NaN compares unequal to
    // itself, which is how the program can tell.
    n = lanes.lane4(lanes.sqrt4(lanes.splat4(-1.0)), 0)
    println("nan ${n != n}")
}
AE

out="$("$AE" run "$tmp/main.ae" 2>&1 | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
expected="sqrt4 3 4 5 1.5
abs4 3.5 0 14.25
sqrt2 9 2.5
abs2 4.5 4.5
exact true
nan true"
if [ "$out" != "$expected" ]; then
    echo "  [FAIL] lane_math: unexpected results"
    printf 'got:\n%s\nwant:\n%s\n' "$out" "$expected" | sed 's/^/        /'
    fail=1
fi

# The four helpers reach the generated C, and BOTH instruction paths are
# still spelled out in it.
#
# This is a source-level check, and it is worth being exact about what it
# does and does not prove. The three branches are `#if`-selected by the C
# compiler, so all three texts are present whichever one this toolchain
# takes -- this cannot say which was chosen. What it does catch is the
# regression that matters: someone deleting or misspelling an instruction
# path, after which the scalar fallback still passes every numeric
# assertion above and the loss shows up only as lost speed. An
# instruction-level assertion was considered and rejected: `sqrtps` does
# not exist on the ARM64 lane, and a -O0 build emits a call on every lane,
# so it would fail for reasons that have nothing to do with this code.
"$AETHERC" "$tmp/main.ae" "$tmp/out.c" >/dev/null 2>&1
for want in '_ae_f32x4_sqrt' '_ae_f64x2_sqrt' '_ae_f32x4_abs' '_ae_f64x2_abs' \
            '__builtin_elementwise_sqrt' '__builtin_ia32_sqrtps' \
            '__builtin_ia32_sqrtpd' '__builtin_sqrtf'; do
    if ! grep -q -- "$want" "$tmp/out.c"; then
        echo "  [FAIL] lane_math: '$want' is missing from the generated C"
        fail=1
    fi
done

if [ "$fail" = 0 ]; then
    echo "  [PASS] lane_math: sqrt4/sqrt2/abs4/abs2 are exact, handle -0.0 and NaN; every lowering path is present in the generated C"
fi
exit $fail
