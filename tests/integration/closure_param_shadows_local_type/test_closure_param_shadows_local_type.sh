#!/bin/sh
# #2453: a closure parameter shadows a same-named local of the enclosing
# function, in the early inference pass as in the type checker.
#
# The early pass keeps one flat table per function, and a closure's
# parameters were never entered into it, so inside `|v: int| { w = v * 2 }`
# the `v` was the enclosing function's `v`. With that one an f32x4, `w` was
# bound as a lane and the program failed with "type mismatch in variable
# initialization". The closure's own locals also leaked into the enclosing
# function's table.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] closure_param_shadows_local_type: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
import std.lanes

main() {
    // An outer lane local, an int parameter of the same name.
    v = lanes.f32x4(1.0, 2.0, 3.0, 4.0)
    f = |v: int| {
        w = v * 2
        println("lane-shadow ${w}")
    }
    call(f, 3)
    println("outer ${v.x}")

    // An outer string, an int parameter of the same name, with op= inside.
    s = "text"
    g = |s: int| {
        s += 10
        t = s * 3
        println("string-shadow ${t}")
    }
    call(g, 1)
    println("outer ${s}")

    // An outer int, a float parameter: arithmetic is float inside.
    n = 7
    h = |n: float| {
        half = n / 2.0
        println("int-shadow ${half}")
    }
    call(h, 5.0)
    println("outer ${n}")
}
AE

want="lane-shadow 6
outer 1
string-shadow 33
outer text
int-shadow 2.5
outer 7"
got="$("$AE" run "$tmp/main.ae" 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] closure_param_shadows_local_type: wrong output"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] closure_param_shadows_local_type: closure parameters shadow outer locals of another type"
