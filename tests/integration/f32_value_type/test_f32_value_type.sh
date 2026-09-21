#!/bin/sh
# `f32`: a 32-bit IEEE float value type (#2134).
#
# `f32` existed only as a spelling for extern tuple fields (#1033): a value
# cast to it failed ("cannot cast float to unknown"), it was not a numeric
# kind, it could not be assigned from a float, printed through %d, and an
# `f32[]` view was unusable. Now it converts with `float` and the integer
# kinds as C converts float and double (a value cast is the explicit
# spelling, assignment performs the same conversion), arithmetic on it is
# `float`, a struct field of it has C's `float` layout, an `f32[]` over a
# `ptr` stores with one C store per element, and it prints as a float.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] f32_value_type: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

cat > "$tmp/main.ae" <<'AE'
struct Viewport { x: f32, y: f32, w: f32, h: f32 }
extern malloc(n: int) -> ptr
extern free(p: ptr)

area(v: Viewport) -> float { return v.w * v.h }
scale(x: f32, k: float) -> f32 { return x * k }

main() {
    f = 1.5
    g = f as f32
    h = g as float
    f32 z = 1.25
    q = z * 2.0
    verts = malloc(32) as f32[]
    i = 0
    while i < 8 { verts[i] = i * 0.5; i = i + 1 }
    verts[0] = 0.25 as f32
    sum = 0.0
    i = 0
    while i < 8 { sum = sum + verts[i]; i = i + 1 }
    v = Viewport { x: 0.0, y: 2.5, w: 3.0, h: 2.0 }
    if z > 1.0 { big = z } else { big = 0.5 }
    println(z)
    print("%f\n", z)
    println("${h} ${v.y} ${q} ${sum} ${area(v)} ${scale(z, 4.0)} ${big} ${z == 1.25} ${verts[7]} ${verts[0]}")
    free(verts)
}
AE
want='1.250000
1.250000
1.5 2.5 2.5 14.25 6 5 1.25 true 3.5 0.25'
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/main.ae" 2>&1)"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] f32_value_type: output differs"
    printf '%s\n' "$got" | head -8 | sed 's/^/        /'
    fail=1
fi

# The C: a `float` field, a `float*` view with plain stores, a float local.
AETHER_HOME="$ROOT" "$AETHERC" "$tmp/main.ae" "$tmp/out.c" >/dev/null 2>&1
for want_c in '    float x;' 'float\* verts = ((float\*)(malloc(32)));' 'float z = 1.25;' 'float scale(float x, double k)'; do
    if ! grep -q "$want_c" "$tmp/out.c"; then
        echo "  [FAIL] f32_value_type: expected '$want_c' in the C"
        fail=1
    fi
done
if grep -q 'printf("%d\n", z)' "$tmp/out.c"; then
    echo "  [FAIL] f32_value_type: an f32 is printed through %d"
    fail=1
fi

# A value cast between f32 and a non-numeric kind is still refused.
printf 'main() {\n    s = "x"\n    g = s as f32\n    println("${g}")\n}\n' > "$tmp/bad.ae"
out="$(AETHER_HOME="$ROOT" "$AETHERC" "$tmp/bad.ae" "$tmp/bad.c" 2>&1)"
if ! printf '%s' "$out" | grep -q "cannot cast string to f32 with \`as\`"; then
    echo "  [FAIL] f32_value_type: string as f32 was not refused by name"
    printf '%s\n' "$out" | grep -E "^error" | head -2 | sed 's/^/        /'
    fail=1
fi

# An f32 into an inferred int is the same narrowing error a float is, and
# a match on an f32 compares as a float (the review found both).
printf 'struct V { x: f32 }
main() {
    v = V { x: 1.5 }
    x = 0
    x = v.x
    println("${x}")
}
' > "$tmp/narrow.ae"
out="$(AETHER_HOME="$ROOT" "$AETHERC" "$tmp/narrow.ae" "$tmp/narrow.c" 2>&1)"
if ! printf '%s' "$out" | grep -q "narrowing assignment to 'x': its type was inferred as int from its initializer, but a float is assigned"; then
    echo "  [FAIL] f32_value_type: an f32 into an inferred int was not refused"
    fail=1
fi
printf 'struct V { x: f32 }
main() {
    v = V { x: 1.5 }
    match v.x {
        1.5 -> { println("m1") }
        _ -> { println("m2") }
    }
}
' > "$tmp/match.ae"
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/match.ae" 2>&1 | tail -1)"
if [ "$got" != "m1" ]; then
    echo "  [FAIL] f32_value_type: a match on an f32 took the wrong arm (got '$got')"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] f32_value_type: f32 converts, computes in float, lays out as C float, stores through f32[]"
fi
exit $fail
