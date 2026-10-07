#!/bin/sh
# A fixed-size array captured by a closure, or held as actor state (#2464).
#
# Both the closure environment struct and the actor struct printed a field
# as `<type> <name>`, and a sized array's C type spells as `int[3]`, so the
# field came out as `int[3] arr;`: "expected identifier or '(' before '['".
# They now spell the declarator `int arr[3]`, as a struct field does. A C
# array does not assign either, so the capture copies it into the
# environment with memcpy, and the actor's spawn zeroes the field and sets
# the elements of an array-literal initializer.
#
# The capture is a snapshot taken when the closure is made: a later write
# to the array in the enclosing function does not reach it.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] fixed_array_capture_and_state: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
message Push { v: int }
message Get {}

actor A {
    state int[4] hist
    state int[3] seed = [7, 8, 9]
    receive {
        Push(v) -> { hist[1] = hist[1] + v }
        Get() -> { reply hist[0] + hist[1] * 10 + seed[2] * 100 }
    }
}

main() {
    int[3] arr = [1, 2, 3]
    g = || -> arr[1] + arr[2]
    arr[1] = 100
    println("g ${call(g)} arr1 ${arr[1]}")

    xs = [4, 5, 6]
    h = || -> xs[0]
    println("h ${call(h)}")

    string[2] names = ["ann", "bo"]
    pick = |i: int| -> names[i]
    println("pick ${call(pick, 1)}")

    outer = || {
        inner = || -> arr[0]
        return call(inner)
    }
    int o = call(outer)
    println("outer ${o}")

    a = spawn(A())
    a ! Push { v: 3 }
    a ! Push { v: 4 }
    n = a ? Get {}
    println("state ${n}")
}
AE

want="g 5 arr1 100
h 4
pick bo
outer 1
state 970"
got="$("$AE" run "$tmp/main.ae" 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] fixed_array_capture_and_state: wrong output"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] fixed_array_capture_and_state: a sized array is captured by copy and held as actor state"
