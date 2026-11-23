#!/bin/sh
# #3815: std.lanes min and max, and the reductions built on them.
#
# min4(a, b) is `a < b ? a : b` per lane and max4 `a > b ? a : b`, which on
# x86 is exactly MINPS(a, b) / MAXPS(a, b): either returns its second
# operand when a lane of either is NaN or both are zero. The helpers were a
# compare and a blend, which GCC never turned into the one instruction, so
# a running minimum carried both on its loop chain. This checks:
#
#   1. min4, max4, min8 and max8 give the bits of the scalar rule for every
#      pair of a set of values with NaNs of both signs, both zeros, both
#      infinities, a subnormal and ordinary numbers, in either operand and
#      in every lane; reduce_min4/max4 and reduce_min8/max8 give the bits of
#      the order the module documents, over every arrangement of a smaller
#      set.
#   2. On x86-64 the four-lane helpers compile to minps / maxps with no
#      blend, and with -mavx2 the eight-lane ones to vminps / vmaxps on ymm.
#      (Elsewhere the compare and select stay: AArch64's fmin returns the
#      NaN and fminnm the number, neither of which is the rule.)
#   3. Where the CPU has AVX2, the [build] cflags = "-mavx2" build prints the
#      same as the default build.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] lane_minmax: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0
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

// IEEE's `<` on two floats given as their bits, in integer arithmetic, so
// the reference shares nothing with the code under test: a NaN is
// unordered, the two zeros are equal, and the rest order by sign and
// magnitude.
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

// The scalar rule, on bits: the first operand where the comparison holds.
pick_min(pa: int, pb: int) -> int {
    if less(pa, pb) { return pa }
    return pb
}

pick_max(pa: int, pb: int) -> int {
    if less(pb, pa) { return pa }
    return pb
}

bits_of(x: f32) -> int {
    return lanes.mask_lane(lanes.bits4(lanes.splat4(x)), 0) as int
}

// Kept out of main so the check below finds each one's instructions.
lmin4(a: f32x4, b: f32x4) -> f32x4 { return lanes.min4(a, b) }
lmax4(a: f32x4, b: f32x4) -> f32x4 { return lanes.max4(a, b) }
lmin8(a: f32x8, b: f32x8) -> f32x8 { return lanes.min8(a, b) }
lmax8(a: f32x8, b: f32x8) -> f32x8 { return lanes.max8(a, b) }

main() {
    wrong4 = 0
    wrong8 = 0
    i = 0
    while i < COUNT {
        j = 0
        while j < COUNT {
            pa = pattern(i)
            pb = pattern(j)
            // Lanes 0 and 2 hold (a, b), lanes 1 and 3 (b, a).
            a = lanes.from_bits4(lanes.i32x4(pa, pb, pa, pb))
            b = lanes.from_bits4(lanes.i32x4(pb, pa, pb, pa))
            mn = lanes.bits4(lmin4(a, b))
            mx = lanes.bits4(lmax4(a, b))
            k = 0
            while k < 4 {
                x = if k % 2 == 0 { pa } else { pb }
                y = if k % 2 == 0 { pb } else { pa }
                if lanes.mask_lane(mn, k) as int != pick_min(x, y) { wrong4 = wrong4 + 1 }
                if lanes.mask_lane(mx, k) as int != pick_max(x, y) { wrong4 = wrong4 + 1 }
                k = k + 1
            }
            a8 = lanes.from_bits8(lanes.i32x8(pa, pb, pa, pb, pb, pa, pb, pa))
            b8 = lanes.from_bits8(lanes.i32x8(pb, pa, pb, pa, pa, pb, pa, pb))
            mn8 = lanes.bits8(lmin8(a8, b8))
            mx8 = lanes.bits8(lmax8(a8, b8))
            k = 0
            while k < 8 {
                first = k == 0 || k == 2 || k == 5 || k == 7
                x = if first { pa } else { pb }
                y = if first { pb } else { pa }
                if lanes.mask8_lane(mn8, k) as int != pick_min(x, y) { wrong8 = wrong8 + 1 }
                if lanes.mask8_lane(mx8, k) as int != pick_max(x, y) { wrong8 = wrong8 + 1 }
                k = k + 1
            }
            j = j + 1
        }
        i = i + 1
    }
    println("min4 max4 wrong ${wrong4}")
    println("min8 max8 wrong ${wrong8}")

    // The reductions, over every arrangement of five of the values (a NaN,
    // both zeros, 1 and -1) in four lanes, and in eight lanes with the
    // second half a rotation of the first.
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
        n = n + 1
    }
    println("reductions wrong ${wrongr}")
}
AE

expected="min4 max4 wrong 0
min8 max8 wrong 0
reductions wrong 0"

if ! (cd "$tmp" && "$AE" build main.ae -o "$tmp/main" >"$tmp/build.log" 2>&1); then
    echo "  [FAIL] lane_minmax: build failed:"
    grep -m5 -iE "error" "$tmp/build.log" | sed 's/^/        /'
    exit 1
fi
out="$("$tmp/main" 2>&1 | tr -d '\r')"
if [ "$out" != "$expected" ]; then
    echo "  [FAIL] lane_minmax: lanes differ from the scalar rule"
    printf 'got:\n%s\nwant:\n%s\n' "$out" "$expected" | sed 's/^/        /'
    fail=1
fi

# A function's body in an assembly file, from its label (Mach-O spells it
# `_lmin4`) to its end of frame: .cfi_endproc (ELF, Mach-O) or .seh_endproc
# (COFF).
body() {
    awk -v f="$1" '$0 ~ "^_?" f "[.a-z0-9]*:" {p=1; next} p && /^[ \t]*\.(cfi|seh)_endproc/ {p=0} p' "$2"
}

case "$(uname -m 2>/dev/null)" in
    x86_64|amd64)
        if "$AETHERC" "$tmp/main.ae" "$tmp/out.c" >/dev/null 2>&1 && command -v gcc >/dev/null 2>&1; then
            if gcc -O2 -fwrapv -S -o "$tmp/out.s" $("$AE" cflags --cflags 2>/dev/null) "$tmp/out.c" 2>"$tmp/s.err"; then
                for f in lmin4:minps lmax4:maxps lmin8:minps lmax8:maxps; do
                    fn=${f%%:*}; ins=${f#*:}
                    b="$(body "$fn" "$tmp/out.s")"
                    if [ -z "$b" ]; then
                        echo "  [FAIL] lane_minmax: $fn is not in the assembly (the check found no function to read)"
                        fail=1
                    elif ! printf '%s\n' "$b" | grep -qE "[[:space:]]v?$ins[[:space:]]"; then
                        echo "  [FAIL] lane_minmax: $fn has no $ins"
                        fail=1
                    elif printf '%s\n' "$b" | grep -qE "blendv|cmp[a-z]*ps|andnps"; then
                        echo "  [FAIL] lane_minmax: $fn still compares and blends"
                        fail=1
                    fi
                done
            else
                echo "  [FAIL] lane_minmax: the generated C did not compile:"
                head -5 "$tmp/s.err" | sed 's/^/        /'
                fail=1
            fi
            if gcc -O2 -mavx2 -fwrapv -S -o "$tmp/avx.s" $("$AE" cflags --cflags 2>/dev/null) "$tmp/out.c" 2>"$tmp/avx.err"; then
                for f in lmin8:vminps lmax8:vmaxps; do
                    fn=${f%%:*}; ins=${f#*:}
                    if ! body "$fn" "$tmp/avx.s" | grep -qE "$ins[^#]*ymm"; then
                        echo "  [FAIL] lane_minmax: with -mavx2 $fn has no $ins on ymm"
                        fail=1
                    fi
                done
            else
                echo "  [FAIL] lane_minmax: the generated C did not compile with -mavx2:"
                head -5 "$tmp/avx.err" | sed 's/^/        /'
                fail=1
            fi
        fi
        cat > "$tmp/probe.c" <<'C'
int main(void) { __builtin_cpu_init(); return __builtin_cpu_supports("avx2") ? 0 : 1; }
C
        if command -v gcc >/dev/null 2>&1 && gcc -o "$tmp/probe" "$tmp/probe.c" 2>/dev/null && "$tmp/probe"; then
            mkdir -p "$tmp/avx"
            cp "$tmp/main.ae" "$tmp/avx/main.ae"
            printf '[package]\nname = "lane_minmax"\nversion = "0.1.0"\n\n[build]\ncflags = "-mavx2"\n' > "$tmp/avx/aether.toml"
            if (cd "$tmp/avx" && "$AE" build main.ae -o "$tmp/avx/main" >"$tmp/avx/build.log" 2>&1); then
                out_avx="$("$tmp/avx/main" 2>&1 | tr -d '\r')"
                if [ "$out_avx" != "$expected" ]; then
                    echo "  [FAIL] lane_minmax: the -mavx2 build differs from the scalar rule"
                    printf 'got:\n%s\n' "$out_avx" | sed 's/^/        /'
                    fail=1
                fi
            else
                echo "  [FAIL] lane_minmax: the [build] cflags = \"-mavx2\" build failed:"
                grep -m5 -iE "error" "$tmp/avx/build.log" | sed 's/^/        /'
                fail=1
            fi
        fi
        ;;
esac

if [ "$fail" -eq 0 ]; then
    echo "  [PASS] lane_minmax: min, max and the reductions are the scalar rule's bits in both forms; minps / maxps on x86"
fi
exit "$fail"
