#!/bin/sh
# #2428: eight float lanes (f32x8) and their mask (i32x8).
#
# aephysics's contact stages run eight lanes wide in Box3D main, and each
# stage roughly halves against four. This checks three things:
#
#   1. Every operation gives the exact value, including -0.0, NaN, both edges
#      of a load, and a sum taken in lane order (so it is the same bits on
#      every instruction set).
#   2. On x86-64, the generated C compiled with -mavx2 is EIGHT-wide: the
#      kernel's multiply and square root are single ymm instructions. Two
#      xmm halves would pass every numeric check above and lose the reason
#      the type exists, so that is asserted at the instruction level here.
#      Without -mavx2 the type is two four-lane halves, and a comparison
#      must stay a packed SSE compare: a plain 32-byte vector was compared
#      one lane at a time (comiss + cmov), 9x slower, and passed every
#      numeric check too. (lane_math explains why its four-lane check stays
#      source-level: the instruction does not exist on ARM. This one only
#      runs on x86-64.)
#   3. Where the CPU has AVX2, a build with aether.toml's `[build] cflags =
#      "-mavx2"` -- the documented way to turn it on -- prints the same
#      results as the default build.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] lane_wide: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
import std.lanes

// Takes and returns the type by value: a real function boundary, which is
// where a 32-byte vector's calling convention matters.
kernel(a: f32x8, b: f32x8) -> f32x8 {
    return lanes.sqrt8(a * b)
}

// A comparison written as an operator, the case the halves form must keep
// packed.
clamp(a: f32x8) -> f32x8 {
    keep = a > 0.0
    return lanes.select8(keep, a, lanes.splat8(0.0))
}

// Values held in heap storage: a slice is calloc'd, 16-byte aligned on
// glibc and macOS. At the vector's natural 32-byte alignment GCC moved
// through it with vmovaps, which faults there.
scale(s: f32x8[], k: f32x8) {
    i = 0
    while i < s.len {
        s[i] = s[i] * k
        i = i + 1
    }
}

main() {
    v = lanes.f32x8(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0)
    println("lanes ${lanes.lane8(v, 0)} ${lanes.lane8(v, 3)} ${lanes.lane8(v, 4)} ${lanes.lane8(v, 7)} xw ${v.x} ${v.w}")

    // Lane-wise arithmetic, with a splat for the scalar.
    w = v * lanes.splat8(2.0) - lanes.splat8(1.0)
    println("arith ${lanes.lane8(w, 0)} ${lanes.lane8(w, 7)} sum ${lanes.sum8(w)} div ${lanes.lane8(v / lanes.splat8(2.0), 7)}")

    // Exact square root and absolute value, every lane.
    r = kernel(lanes.f32x8(1.0, 4.0, 9.0, 16.0, 25.0, 36.0, 49.0, 64.0), lanes.splat8(1.0))
    println("sqrt ${lanes.lane8(r, 0)} ${lanes.lane8(r, 3)} ${lanes.lane8(r, 4)} ${lanes.lane8(r, 7)}")
    a = lanes.abs8(lanes.f32x8(-1.5, 2.5, -0.0, -4.0, 5.0, -6.5, 7.0, -8.0))
    println("abs ${lanes.lane8(a, 0)} ${lanes.lane8(a, 2)} ${lanes.lane8(a, 7)} sum ${lanes.sum8(a)}")
    n = lanes.lane8(lanes.sqrt8(lanes.splat8(-1.0)), 5)
    println("nan ${n != n}")

    // Comparison -> mask -> select, in the upper half as well as the lower.
    m = v > lanes.splat8(4.5)
    s = lanes.select8(m, v, lanes.splat8(0.0))
    println("select ${lanes.lane8(s, 3)} ${lanes.lane8(s, 4)} ${lanes.lane8(s, 7)} mask ${lanes.mask8_lane(m, 3)} ${lanes.mask8_lane(m, 4)}")
    println("any ${lanes.any8(m)} all ${lanes.all8(m)} none ${lanes.any8(lanes.lt8(v, lanes.splat8(0.0)))} every ${lanes.all8(lanes.ge8(v, lanes.splat8(1.0)))}")
    both = lanes.mask8_and(m, lanes.lt8(v, lanes.splat8(7.5)))
    either = lanes.mask8_or(lanes.eq8(v, lanes.splat8(1.0)), lanes.eq8(v, lanes.splat8(8.0)))
    println("masks ${lanes.mask8_lane(both, 6)} ${lanes.mask8_lane(both, 7)} ${lanes.mask8_lane(either, 0)} ${lanes.mask8_lane(either, 7)} ${lanes.mask8_lane(lanes.mask8_not(either), 7)}")
    lo = lanes.min8(v, lanes.splat8(4.0))
    hi = lanes.max8(v, lanes.splat8(4.0))
    println("minmax ${lanes.lane8(lo, 7)} ${lanes.lane8(hi, 0)} le ${lanes.mask8_lane(lanes.le8(v, lanes.splat8(4.0)), 3)} gt ${lanes.mask8_lane(lanes.gt8(v, lanes.splat8(4.0)), 3)}")

    // Compound assignment, unary minus and `~`, and integer-lane arithmetic:
    // every operator on these types goes through a helper.
    acc = lanes.splat8(1.0)
    acc += v
    acc *= 2.0
    neg = -acc
    inv = ~m
    q = m * 2
    rq = q % 3
    println("ops ${acc.x} ${lanes.lane8(acc, 7)} ${lanes.lane8(neg, 7)} ${lanes.sum8(clamp(neg))} ${lanes.sum8(clamp(acc))} ${inv.x} ${lanes.mask8_lane(inv, 7)} ${lanes.mask8_lane(q, 4)} ${lanes.mask8_lane(rq, 4)}")

    // A lane write, plain and compound: an lvalue in both forms.
    v2 = v
    v2.x = 9.0
    v2.w += 1.0
    println("write ${v2.x} ${v2.w} sum ${lanes.sum8(v2)}")

    // A closure parameter that shadows an f32x8 local: its `+=` is on the
    // int parameter, not a lane helper.
    bump = |v: int| {
        v += 1
        println("shadow ${v}")
    }
    call(bump, 1)

    // f32x8 values in a heap slice.
    heap = make([]f32x8, 3)
    heap[0] = v
    heap[2] = lanes.splat8(1.5)
    scale(heap, lanes.splat8(2.0))
    println("heap ${lanes.sum8(heap[0])} ${lanes.sum8(heap[2])}")
    free(heap)

    // A load and a store at an element offset, through a checked slice.
    buf = make([]f32, 10)
    lanes.store8_slice(buf, 2, v)
    back = lanes.load8_slice(buf, 2)
    println("slice ${buf[1]} ${buf[2]} ${buf[9]} ${lanes.sum8(back)}")
    caught = false
    try {
        _x = lanes.load8_slice(buf, 3)   // needs elements 3..10, buf has 10
    } catch e {
        caught = true
    }
    println("bounds ${caught}")
    free(buf)
}
AE

expected="lanes 1 4 5 8 xw 1 4
arith 1 15 sum 64 div 4
sqrt 1 4 5 8
abs 1.5 0 8 sum 34.5
nan true
select 0 5 8 mask 0 -1
any true all false none false every true
masks -1 0 -1 -1 0
minmax 4 4 le -1 gt 0
ops 4 18 -18 0 88 -1 0 -2 -2
write 9 5 sum 45
shadow 2
heap 72 24
slice 0 1 8 36
bounds true"

filter() { grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$"; }

out="$("$AE" run "$tmp/main.ae" 2>&1 | tr -d '\r' | filter)"
if [ "$out" != "$expected" ]; then
    echo "  [FAIL] lane_wide: unexpected results"
    printf 'got:\n%s\nwant:\n%s\n' "$out" "$expected" | sed 's/^/        /'
    fail=1
fi

arch="$(uname -m 2>/dev/null)"
case "$arch" in
    x86_64|amd64|AMD64)
        # 2. Instruction-level width check on the generated C.
        if "$AETHERC" "$tmp/main.ae" "$tmp/out.c" >/dev/null 2>&1 && command -v gcc >/dev/null 2>&1; then
            if gcc -O2 -mavx2 -fwrapv -S -o "$tmp/out.s" $("$AE" cflags --cflags 2>/dev/null) "$tmp/out.c" 2>"$tmp/s.err"; then
                # macOS prefixes C symbols with an underscore.
                kern="$(sed -n '/^_\{0,1\}kernel:/,/ret/p' "$tmp/out.s")"
                # Heap storage is 16-byte aligned, so no aligned 32-byte move
                # may go through a slice element. This bites on the Linux and
                # macOS legs: MinGW's GCC emits vmovups here whatever the
                # type's alignment, so on Windows it cannot fail.
                sc="$(sed -n '/^_\{0,1\}scale:/,/ret/p' "$tmp/out.s")"
                if printf '%s\n' "$sc" | grep -qE 'vmovaps[^;]*%ymm'; then
                    echo "  [FAIL] lane_wide: with -mavx2, scale moves a slice element with vmovaps (faults on 16-byte-aligned heap)"
                    printf '%s\n' "$sc" | grep -E 'vmovaps' | head -3 | sed 's/^/        /'
                    fail=1
                fi
                if ! printf '%s\n' "$sc" | grep -qE 'vmulps.*%ymm'; then
                    echo "  [FAIL] lane_wide: with -mavx2, scale is not eight-wide"
                    fail=1
                fi
                for want in 'vmulps.*%ymm' 'vsqrtps.*%ymm'; do
                    if ! printf '%s\n' "$kern" | grep -qE "$want"; then
                        echo "  [FAIL] lane_wide: with -mavx2 the kernel has no '$want' (not eight-wide)"
                        printf '%s\n' "$kern" | head -20 | sed 's/^/        /'
                        fail=1
                    fi
                done
            else
                echo "  [FAIL] lane_wide: the generated C did not compile with -mavx2:"
                head -5 "$tmp/s.err" | sed 's/^/        /'
                fail=1
            fi
            # Without AVX2: two halves, and the comparison packed.
            if gcc -O2 -fwrapv -S -o "$tmp/out_sse.s" $("$AE" cflags --cflags 2>/dev/null) "$tmp/out.c" 2>"$tmp/sse.err"; then
                cl="$(sed -n '/^_\{0,1\}clamp:/,/ret/p' "$tmp/out_sse.s")"
                if ! printf '%s\n' "$cl" | grep -qE 'cmp[a-z]*ps'; then
                    echo "  [FAIL] lane_wide: without -mavx2, clamp has no packed compare (cmp*ps)"
                    printf '%s\n' "$cl" | head -20 | sed 's/^/        /'
                    fail=1
                fi
                if printf '%s\n' "$cl" | grep -qE 'u?comiss'; then
                    echo "  [FAIL] lane_wide: without -mavx2, clamp compares one lane at a time (comiss)"
                    fail=1
                fi
            else
                echo "  [FAIL] lane_wide: the generated C did not compile without -mavx2:"
                head -5 "$tmp/sse.err" | sed 's/^/        /'
                fail=1
            fi
        fi

        # 3. The aether.toml route, where this CPU can run it.
        cat > "$tmp/probe.c" <<'C'
int main(void) { __builtin_cpu_init(); return __builtin_cpu_supports("avx2") ? 0 : 1; }
C
        if command -v gcc >/dev/null 2>&1 && gcc -o "$tmp/probe" "$tmp/probe.c" 2>/dev/null && "$tmp/probe"; then
            mkdir -p "$tmp/avx"
            cp "$tmp/main.ae" "$tmp/avx/main.ae"
            printf '[package]\nname = "lane_wide"\nversion = "0.1.0"\n\n[build]\ncflags = "-mavx2"\n' > "$tmp/avx/aether.toml"
            if (cd "$tmp/avx" && "$AE" build main.ae -o "$tmp/avx/main" >"$tmp/avx/build.log" 2>&1); then
                out_avx="$("$tmp/avx/main" 2>&1 | tr -d '\r' | filter)"
                if [ "$out_avx" != "$expected" ]; then
                    echo "  [FAIL] lane_wide: the -mavx2 build prints different results"
                    printf 'got:\n%s\n' "$out_avx" | sed 's/^/        /'
                    fail=1
                fi
            else
                echo "  [FAIL] lane_wide: the [build] cflags = \"-mavx2\" build failed:"
                grep -m5 -iE "error" "$tmp/avx/build.log" | sed 's/^/        /'
                fail=1
            fi
        else
            echo "  [SKIP-AVX2] lane_wide: this CPU has no AVX2; the -mavx2 run is skipped (the instruction check above still ran)"
        fi
        ;;
esac

if [ "$fail" = 0 ]; then
    echo "  [PASS] lane_wide: f32x8/i32x8 give exact results; eight-wide (ymm) with -mavx2, packed halves without, on x86-64"
fi
exit $fail
