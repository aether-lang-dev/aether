#!/bin/sh
# #2151: arithmetic on f32 operands is done in C float.
#
# 0.704's f32 was a storage type: a field or an f32[] element was a C
# float, but `a * b` on two of them was computed in double and narrowed at
# the store, so a physics engine's vector maths — every op the reference
# does in float — ran in double, half the width and twice the bytes. Now
# f32 op f32 and f32 op <integer> are f32 (C's usual conversions), a numeric
# LITERAL beside an f32 operand takes the f32 (`v * 0.5` is one float
# multiply; the literal is emitted as `0.5f`), and a float (double) VALUE on
# the other side still widens to float, as in C.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] f32_arithmetic: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
struct Vec3f { x: f32, y: f32, z: f32 }
half(v: f32) -> f32 { return v * 0.5 }        // a one-line return, the physics shape
nhalf(v: f32) -> f32 { return v * -0.5 }
below(v: f32) -> bool { return v < 0.5 }
dot(a: Vec3f, b: Vec3f) -> f32 { return a.x * b.x + a.y * b.y + a.z * b.z }
scale(a: Vec3f, k: f32) -> Vec3f { return Vec3f { x: a.x * k, y: a.y * k, z: a.z * k } }
main() {
    a = 0.1 as f32
    b = 3 as f32
    c = 0.3 as f32
    // In float, 0.1f * 3.0f rounds to exactly 0.3f; in double, 0.1f * 3
    // is 0.30000000447 and 0.3f is 0.30000001192 — the comparison tells
    // which precision the multiply used.
    println("float-exact ${a * b == c}")
    h = a * 0.5           // literal takes the f32: a float multiply
    g = 2.0 * a           // literal on the left too
    d = a * 1.5 + 2       // an integer operand stays f32
    n = a * 4             // f32 * int
    q = 0.5               // a float (double) VALUE
    m = a * q             // widens to float, as in C
    e = a * -0.5          // a negated literal is still a constant
    v = Vec3f { x: 1.0, y: 2.0, z: 3.0 }
    println("${dot(v, v)} ${scale(v, 0.5).y} ${h} ${g} ${d} ${n} ${m} ${e} ${half(a)} ${nhalf(a)} ${below(a)}")
}
AE

got="$("$AE" run "$tmp/main.ae" 2>&1 | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$" | tr '\n' ' ')"
if [ "$got" != "float-exact true 14 1 0.05 0.2 2.15 0.4 0.05 -0.05 0.05 -0.05 true " ]; then
    echo "  [FAIL] f32_arithmetic: program output (got '$got')"
    fail=1
fi

"$AETHERC" "$tmp/main.ae" "$tmp/out.c" >/dev/null 2>&1
# the locals: float where every operand is f32/int/literal, double with a double value
for want in 'float h = ' 'float g = ' 'float d = ' 'float n = ' 'double m = ' 'float e = ' 'float dot(Vec3f a, Vec3f b)'; do
    if ! grep -q "$want" "$tmp/out.c"; then
        echo "  [FAIL] f32_arithmetic: generated C lacks '$want'"
        fail=1
    fi
done
# the literals carry C's f suffix, in a return and a comparison too
for want in '0.5f' '1.5f' '2.0f' 'return (v \* 0.5f)' 'return (v \* (-(0.5f)))' 'return (v < 0.5f)'; do
    if ! grep -q "$want" "$tmp/out.c"; then
        echo "  [FAIL] f32_arithmetic: literal beside an f32 not emitted as $want"
        fail=1
    fi
done

# std.math's single-precision family: f32 in, f32 out, libm's f functions
cat > "$tmp/m.ae" <<'AE'
import std.math
main() {
    v = 9 as f32
    r = math.sqrt_f32(v) * 0.5
    println("${r} ${math.clamp_f32(v, 0, 4)} ${math.atan2_f32(0, 1)} ${math.abs_f32(-2.5 as f32)} ${math.max_f32(v, 10)}")
}
AE
got="$("$AE" run "$tmp/m.ae" 2>&1 | tail -1)"
if [ "$got" != "1.5 4 0 2.5 10" ]; then
    echo "  [FAIL] f32_arithmetic: std.math f32 functions (got '$got')"
    fail=1
fi
"$AETHERC" "$tmp/m.ae" "$tmp/m.c" >/dev/null 2>&1
if ! grep -q "float r = (math_sqrt_f32(v) \* 0.5f)" "$tmp/m.c"; then
    echo "  [FAIL] f32_arithmetic: math.sqrt_f32 result is not a float local"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] f32_arithmetic: f32 op f32/int/literal is a C float op (literals get the f suffix); a double value still widens"
fi
exit $fail
