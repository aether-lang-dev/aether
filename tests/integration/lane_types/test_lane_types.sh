#!/bin/sh
# #2146: the f32x4 / f64x2 / i32x4 lane types and std.lanes.
#
# A contact solver matching its C reference needs four floats in one
# register: lane-wise `+ - * /`, a comparison that yields a mask, a select
# that picks per lane, min/max, and unaligned loads/stores over a plain
# buffer. The types lower to the GCC/Clang vector extensions
# (`__attribute__((vector_size(16)))`) and every std.lanes entry point is a
# `static inline` helper in the same translation unit, so a lane operation
# is the instruction it names rather than a call.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] lane_types: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
import std.lanes
extern malloc(n: int) -> ptr
extern free(p: ptr)

// The shape the ask names: a lane-wise kernel with a select.
clamp_lanes(v: f32x4, lo: f32x4, hi: f32x4) -> f32x4 {
    return lanes.min4(lanes.max4(v, lo), hi)
}

main() {
    a = lanes.f32x4(1.0, 2.0, 3.0, 4.0)
    b = lanes.splat4(2.0)
    c = a * b + lanes.splat4(1.0)         // lane-wise multiply-add
    println("arith ${c.x} ${c.y} ${c.z} ${c.w} ${lanes.sum4(c)}")

    d = c - a
    e = c / b
    println("subdiv ${lanes.sum4(d)} ${e.x} ${e.w}")

    m = c > lanes.splat4(5.0)              // a comparison is a MASK
    picked = lanes.select4(m, c, lanes.splat4(0.0))
    println("mask ${m.x} ${m.z} ${lanes.any4(m)} ${lanes.all4(m)} ${lanes.sum4(picked)}")

    cl = clamp_lanes(a, lanes.splat4(2.0), lanes.splat4(3.0))
    println("clamp ${cl.x} ${cl.y} ${cl.z} ${cl.w}")

    // loads and stores over a plain buffer, at an element offset
    raw = malloc(64)
    buf = raw as f32[]
    i = 0
    while i < 8 { buf[i] = (i + 1) as f32; i = i + 1 }
    v0 = lanes.load4(raw, 0)
    v1 = lanes.load4(raw, 4)
    lanes.store4(raw, 0, v0 + v1)
    println("mem ${lanes.sum4(v0)} ${lanes.sum4(v1)} ${buf[0] as float} ${buf[3] as float}")
    free(raw)

    // two double lanes
    p = lanes.f64x2(1.5, 2.5)
    q = lanes.splat2(2.0)
    r = p * q
    println("f64x2 ${r.x} ${r.y} ${lanes.sum2(lanes.min2(p, q))} ${lanes.lane2(r, 1)}")

    // masks compose
    m2 = lanes.mask_and(m, lanes.lt4(c, lanes.splat4(9.0)))
    println("masks ${lanes.mask_lane(m2, 2)} ${lanes.mask_lane(m2, 3)} ${lanes.any4(lanes.mask_not(m))}")

    // a scalar splats in a comparison as it does in arithmetic
    sm = a > 2.0
    println("splat ${lanes.mask_lane(sm, 0)} ${lanes.mask_lane(sm, 3)} ${lanes.all4(a > 0.0)}")

    // an f64x2 comparison yields a TWO-lane mask of its own width: an
    // i32x4 there would reinterpret the register and scramble the lanes
    pm = p > lanes.splat2(2.0)   // 1.5 no, 2.5 yes
    println("f64mask ${lanes.mask2_lane(pm, 0)} ${lanes.mask2_lane(pm, 1)} ${lanes.any2(pm)} ${lanes.all2(pm)} ${lanes.sum2(lanes.select2(pm, p, q))}")
}
AE
want='arith 3 5 7 9 24
subdiv 14 1.5 4.5
mask 0 -1 true false 16
clamp 2 2 3 3
mem 10 26 6 12
f64x2 3 5 3.5 5
masks -1 0 true
splat 0 -1 true
f64mask 0 -1 true false 4.5'
got="$("$AE" run "$tmp/main.ae" 2>&1 | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] lane_types: program output differs"
    printf '%s\n' "$got" | head -10 | sed 's/^/        /'
    fail=1
fi

# The emitted C: real vector types, and the arithmetic is one lane-wise
# expression rather than four scalar ones.
"$AETHERC" "$tmp/main.ae" "$tmp/out.c" >/dev/null 2>&1
for want_c in 'typedef float  AeF32x4 __attribute__((vector_size(16)))' \
              'typedef double AeF64x2 __attribute__((vector_size(16)))' \
              'AeF32x4 clamp_lanes(AeF32x4 v, AeF32x4 lo, AeF32x4 hi)'; do
    if ! grep -qF "$want_c" "$tmp/out.c"; then
        echo "  [FAIL] lane_types: generated C lacks: $want_c"
        fail=1
    fi
done
if ! grep -qF '(c)[0]' "$tmp/out.c"; then
    echo "  [FAIL] lane_types: a lane read (.x) did not lower to a vector index"
    fail=1
fi

# A lane type is nominal: no implicit conversion to or from a scalar, and
# `.z` on a two-lane vector is refused.
cat > "$tmp/bad1.ae" <<'AE'
import std.lanes
main() {
    a = lanes.f32x4(1.0, 2.0, 3.0, 4.0)
    f32 s = a
    println("${s}")
}
AE
if "$AE" run "$tmp/bad1.ae" >/dev/null 2>&1; then
    echo "  [FAIL] lane_types: an f32x4 was accepted where an f32 was declared"
    fail=1
fi
cat > "$tmp/bad2.ae" <<'AE'
import std.lanes
main() {
    d = lanes.f64x2(1.5, 2.5)
    println("${d.z}")
}
AE
out="$("$AE" run "$tmp/bad2.ae" 2>&1)"
if ! printf '%s\n' "$out" | grep -q "is not a lane of f64x2"; then
    echo "  [FAIL] lane_types: '.z' on an f64x2 was not refused at the source"
    printf '%s\n' "$out" | head -3 | sed 's/^/        /'
    fail=1
fi
cat > "$tmp/bad3.ae" <<'AE'
import std.lanes
main() {
    a = lanes.f32x4(1.0, 2.0, 3.0, 4.0)
    d = lanes.f64x2(1.5, 2.5)
    println("${lanes.sum4(a + d)}")
}
AE
if "$AE" run "$tmp/bad3.ae" >/dev/null 2>&1; then
    echo "  [FAIL] lane_types: f32x4 + f64x2 was accepted"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] lane_types: lane-wise arithmetic, masks and select, min/max, loads/stores, f64x2, lane reads; mixing widths or scalars is refused"
fi
exit $fail
