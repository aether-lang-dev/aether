#!/bin/sh
# #3848: a float `<`, `<=`, `>` or `>=` false because of a NaN.
#
# GCC (15.2 and 16.2, at -O1 and up) took the false edge of `a > n` for
# the relation `a <= n` and chained such relations: from `a > n` and
# `n > b` both false, with n a NaN, it folded `a > b` to false. The
# lane_minmax test's pick_max(pa, pb), `value(pa) > value(pb)` over floats
# read out of lanes, returned what pick_min returns for NaNs, the zeros
# and +-1. ae now gives GCC `--param relation-block-limit=0`, so it records
# no relation to chain (gcc_relation_limit checks the flag). This builds
# with `ae build` (-O2) and checks, against integer arithmetic on the bits:
#
#   1. lane_minmax's reductions loop as #3848 found it: pick_min / pick_max
#      by float comparison of every arrangement of a NaN, both zeros, 1
#      and -1, beside the lane reductions, and every pair of fourteen
#      values;
#   2. a > n, n > b, a > b (and with <, <=, >=) on f32 and float, n a NaN
#      read at run time: the third holds;
#   3. a float match range, whose bounds compare through the same macros.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] float_order_nan: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
import std.lanes

// The values, as their bits: quiet NaN, negative quiet NaN, a NaN with a
// payload, +0, -0, 1, -1, 2.5, -2.5, +inf, -inf, the smallest subnormal,
// 3e38 and -1e-30.
const COUNT = 14

pattern(i: int) -> int {
    if i == 0 { return 2143289344 }     // 0x7fc00000
    if i == 1 { return -4194304 }       // 0xffc00000
    if i == 2 { return 2143289345 }     // 0x7fc00001
    if i == 3 { return 0 }
    if i == 4 { return -2147483648 }    // 0x80000000
    if i == 5 { return 1065353216 }     // 1.0
    if i == 6 { return -1082130432 }    // -1.0
    if i == 7 { return 1075838976 }     // 2.5
    if i == 8 { return -1071644672 }    // -2.5
    if i == 9 { return 2139095040 }     // +inf
    if i == 10 { return -8388608 }      // -inf
    if i == 11 { return 1 }             // smallest subnormal
    if i == 12 { return 2135127772 }    // about 3e38
    return -1206807282                  // about -1e-30
}

// A NaN, +0, -0, 1 and -1.
five(i: int) -> int {
    if i == 0 { return pattern(0) }
    if i == 1 { return pattern(3) }
    if i == 2 { return pattern(4) }
    if i == 3 { return pattern(5) }
    return pattern(6)
}

// IEEE's `<` on two floats given as their bits, in integer arithmetic: a
// NaN is unordered, the two zeros are equal, and the rest order by sign
// and magnitude.
is_nan(p: int) -> bool {
    return (p & 2147483647) > 2139095040
}

order_key(p: int) -> int {
    if p < 0 { return 0 - (p & 2147483647) }
    return p
}

less(pa: int, pb: int) -> bool {
    if is_nan(pa) || is_nan(pb) { return false }
    return order_key(pa) < order_key(pb)
}

ref_min(pa: int, pb: int) -> int {
    if less(pa, pb) { return pa }
    return pb
}

ref_max(pa: int, pb: int) -> int {
    if less(pb, pa) { return pa }
    return pb
}

// The code under test: the same rule by float comparison.
value(p: int) -> f32 {
    return lanes.lane4(lanes.from_bits4(lanes.i32x4(p, p, p, p)), 0)
}

pick_min(pa: int, pb: int) -> int {
    if value(pa) < value(pb) { return pa }
    return pb
}

pick_max(pa: int, pb: int) -> int {
    if value(pa) > value(pb) { return pa }
    return pb
}

bits_of(x: f32) -> int {
    return lanes.mask_lane(lanes.bits4(lanes.splat4(x)), 0) as int
}

// a OP n, n OP b, a OP b: with n a NaN the first two are false, which
// says nothing of the third.
gt32(a: f32, n: f32, b: f32) -> int {
    if a > n { return 1 }
    if n > b { return 2 }
    if a > b { return 3 }
    return 4
}

ge32(a: f32, n: f32, b: f32) -> int {
    if a >= n { return 1 }
    if n >= b { return 2 }
    if a >= b { return 3 }
    return 4
}

lt32(a: f32, n: f32, b: f32) -> int {
    if a < n { return 1 }
    if n < b { return 2 }
    if a < b { return 3 }
    return 4
}

le32(a: f32, n: f32, b: f32) -> int {
    if a <= n { return 1 }
    if n <= b { return 2 }
    if a <= b { return 3 }
    return 4
}

gt64(a: float, n: float, b: float) -> int {
    if a > n { return 1 }
    if n > b { return 2 }
    if a > b { return 3 }
    return 4
}

lt64(a: float, n: float, b: float) -> int {
    if a < n { return 1 }
    if n < b { return 2 }
    if a < b { return 3 }
    return 4
}

band(x: float) -> int {
    return match x {
        0.0..<1.0 -> 1
        1.0..=2.0 -> 2
        _ -> 3
    }
}

main() {
    // 1. Every pair.
    wrongp = 0
    i = 0
    while i < COUNT {
        j = 0
        while j < COUNT {
            pa = pattern(i)
            pb = pattern(j)
            if pick_min(pa, pb) != ref_min(pa, pb) { wrongp = wrongp + 1 }
            if pick_max(pa, pb) != ref_max(pa, pb) { wrongp = wrongp + 1 }
            j = j + 1
        }
        i = i + 1
    }
    println("pairs wrong ${wrongp}")

    // lane_minmax's reductions loop, the shape #3848 was found in.
    wrongr = 0
    n = 0
    while n < 625 {
        q0 = five(n % 5)
        q1 = five((n / 5) % 5)
        q2 = five((n / 25) % 5)
        q3 = five(n / 125)
        v = lanes.from_bits4(lanes.i32x4(q0, q1, q2, q3))
        want_min = pick_min(pick_min(q0, q2), pick_min(q1, q3))
        want_max = pick_max(pick_max(q0, q2), pick_max(q1, q3))
        if want_min != ref_min(ref_min(q0, q2), ref_min(q1, q3)) { wrongr = wrongr + 1 }
        if want_max != ref_max(ref_max(q0, q2), ref_max(q1, q3)) { wrongr = wrongr + 1 }
        if bits_of(lanes.reduce_min4(v)) != want_min { wrongr = wrongr + 1 }
        if bits_of(lanes.reduce_max4(v)) != want_max { wrongr = wrongr + 1 }
        v8 = lanes.from_bits8(lanes.i32x8(q0, q1, q2, q3, q3, q0, q1, q2))
        m0 = pick_min(q0, q3)
        m1 = pick_min(q1, q0)
        m2 = pick_min(q2, q1)
        m3 = pick_min(q3, q2)
        x0 = pick_max(q0, q3)
        x1 = pick_max(q1, q0)
        x2 = pick_max(q2, q1)
        x3 = pick_max(q3, q2)
        if bits_of(lanes.reduce_min8(v8)) != pick_min(pick_min(m0, m2), pick_min(m1, m3)) { wrongr = wrongr + 1 }
        if bits_of(lanes.reduce_max8(v8)) != pick_max(pick_max(x0, x2), pick_max(x1, x3)) { wrongr = wrongr + 1 }
        if x3 != ref_max(q3, q2) { wrongr = wrongr + 1 }
        if m3 != ref_min(q3, q2) { wrongr = wrongr + 1 }
        n = n + 1
    }
    println("reductions wrong ${wrongr}")

    // 2. The chains, with a NaN made at run time.
    zero = 0.0
    nan = zero / zero
    one = zero + 1.0
    nan32 = nan as f32
    one32 = one as f32
    zero32 = zero as f32
    println("f32 ${gt32(one32, nan32, zero32)} ${ge32(one32, nan32, zero32)} ${lt32(zero32, nan32, one32)} ${le32(zero32, nan32, one32)}")
    println("float ${gt64(one, nan, zero)} ${lt64(zero, nan, one)}")

    // 3. A float match range.
    println("band ${band(zero + 0.5)} ${band(one + one)} ${band(nan)}")
}
AE

expected="pairs wrong 0
reductions wrong 0
f32 3 3 3 3
float 3 3
band 1 2 3"

if ! (cd "$tmp" && "$AE" build main.ae -o "$tmp/main" >"$tmp/build.log" 2>&1); then
    echo "  [FAIL] float_order_nan: build failed:"
    grep -m5 -iE "error" "$tmp/build.log" | sed 's/^/        /'
    exit 1
fi
out="$("$tmp/main" 2>&1 | tr -d '\r')"
if [ "$out" != "$expected" ]; then
    echo "  [FAIL] float_order_nan: a float comparison folded against a NaN"
    printf 'got:\n%s\nwant:\n%s\n' "$out" "$expected" | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] float_order_nan: float comparisons beside a NaN keep IEEE's answer at -O2"
exit 0
