#!/bin/sh
# A struct argument of the wrong struct type is a type error (#2491).
#
# Argument checking for user functions is lenient, and a struct passed where
# the parameter takes another struct went through to gcc: "incompatible type
# for argument 1 of 'length2'", one error per compile, against generated
# code. The same values in an assignment were already a type error. Every
# mismatched argument is now reported at the argument, all in one pass, and
# the valid shapes (the same struct, a module's struct, `*T` and `ptr`
# parameters, a variant into its sum type) still compile and run.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] struct_argument_mismatch: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"
cd "$tmp" || exit 1

fail() {
    echo "  [FAIL] struct_argument_mismatch: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2" | head -24
    exit 1
}

# The issue's program.
cat > issue.ae <<'AE'
struct Wide {
    x: float
    y: float
}

struct Narrow {
    x: f32
    y: f32
}

length2(a: Wide) -> float { return a.x * a.x + a.y * a.y }

main() {
    n = Narrow { x: 3.0, y: 4.0 }
    println("${length2(n)}")
}
AE
if "$AE" check issue.ae > issue.log 2>&1; then
    fail "a Narrow passed for a Wide was accepted" issue.log
fi
tr -d '\r' < issue.log > issue.txt
grep -qF "Argument 1 'a' of 'length2': expected Wide, got Narrow" issue.txt ||
    fail "missing the argument diagnostic" issue.txt
grep -qF -- "--> issue.ae:15:24" issue.txt || fail "not reported at the argument" issue.txt
if grep -q "incompatible type for argument" issue.txt; then
    fail "still reported by the C compiler" issue.txt
fi

mkdir -p geo
cat > geo/module.ae <<'AE'
exports ( Vec3, Vec3f, Circle, Rect, Shape, dot, scale, area, origin )

struct Vec3 {
    x: float
    y: float
    z: float
}

struct Vec3f {
    x: f32
    y: f32
    z: f32
}

struct Circle { r: int }
struct Rect { w: int, h: int }
type Shape = Circle | Rect

dot(a: Vec3, b: Vec3) -> float { return a.x * b.x + a.y * b.y + a.z * b.z }

scale(p: *Vec3, k: float) {
    p.x = p.x * k
    p.y = p.y * k
    p.z = p.z * k
}

area(s: Shape) -> int {
    match s {
        Circle -> { return 3 }
        Rect -> { return 12 }
    }
}

origin() -> Vec3 { return Vec3 { x: 0.0, y: 0.0, z: 0.0 } }
AE

# A module's struct where its other struct is expected, in both argument
# positions: both are listed in the one pass.
cat > module_bad.ae <<'AE'
import geo

main() {
    f = Vec3f { x: 1.0, y: 2.0, z: 3.0 }
    println("${geo.dot(f, f)}")
}
AE
if "$AE" check module_bad.ae > module_bad.log 2>&1; then
    fail "a Vec3f passed for a module's Vec3 was accepted" module_bad.log
fi
tr -d '\r' < module_bad.log > module_bad.txt
grep -qF "Argument 1 'a' of 'geo.dot': expected Vec3, got Vec3f" module_bad.txt ||
    fail "missing argument 1" module_bad.txt
grep -qF "Argument 2 'b' of 'geo.dot': expected Vec3, got Vec3f" module_bad.txt ||
    fail "missing argument 2" module_bad.txt

# The valid shapes still compile and run.
cat > good.ae <<'AE'
import geo

struct Pair {
    a: int
    b: int
}

sum_pair(p: Pair) -> int { return p.a + p.b }

through_ref(p: *Pair) -> int { return p.a * p.b }

non_null(p: ptr) -> int {
    if p == null { return 0 }
    return 1
}

main() {
    v = Vec3 { x: 1.0, y: 2.0, z: 3.0 }
    println("${geo.dot(v, v)}")
    println("${geo.dot(geo.origin(), v)}")
    geo.scale(&v, 2.0)
    println("${v.x} ${v.y} ${v.z}")
    println("${geo.area(Circle { r: 1 })} ${geo.area(Rect { w: 3, h: 4 })}")
    p = Pair { a: 2, b: 5 }
    println("${sum_pair(p)} ${through_ref(&p)} ${non_null(&p)}")
}
AE
want="14
0
2 4 6
3 12
7 10 1"
got="$("$AE" run good.ae 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] struct_argument_mismatch: a valid struct argument stopped working"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi

echo "  [PASS] struct_argument_mismatch: a wrong struct argument is a type error; valid struct arguments still run"
