#!/bin/sh
# An array literal's element type is the numeric join of ALL its elements,
# decided once they are typed (#2119).
#
# The early inference pass chose the element type from the first element
# before anything imported could be resolved, so `[0.18 * math.PI, ...]`
# with a module constant came out as an int array (the product's type was
# unknown then, and unknown lowers to int) and every element was truncated
# — a sweep of angles ran at 0 or 1 with no diagnostic. `[1, 2.5, 3]` had
# the same fate from its first element. Three places had to agree: the
# literal's type, the inferred declaration that adopts it, and an index
# expression that was stamped with the stale element type and reached
# printf as `%d` on a double slot.
#
# Also here, since they are the same slot: a float assigned to an int
# variable or int-array element whose type was INFERRED is the narrowing
# error #698 gives for a 64-bit value (it used to truncate silently and,
# for the variable, print garbage through `%g`), and a nested array
# literal is refused instead of emitting invalid C.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] array_literal_element_type: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

mkdir -p "$tmp/lib/mathx"
cat > "$tmp/lib/mathx/module.ae" <<'AE'
exports(PI, half)

const PI = 3.14159265359

half(x: float) -> float { return x * 0.5 }
AE
cat > "$tmp/main.ae" <<'AE'
import mathx

main() {
    rolls = [ 0.18 * mathx.PI, 0.25 * mathx.PI, 0.32 * mathx.PI ]
    println("rolls ${rolls[0]} ${rolls[1]} ${rolls[2]}")
    mixed = [ 1, 2.5, 3 ]
    println("mixed ${mixed[0]} ${mixed[1]} ${mixed[2]}")
    calls = [ mathx.half(1.0), mathx.half(3.0) ]
    println("calls ${calls[0]} ${calls[1]}")
    ints = [ 1, 2, 3 ]
    println("ints ${ints[0]} ${ints[2]}")
    longs = [ 4000000000, 1 ]
    println("longs ${longs[0]} ${longs[1]}")
    k = mixed[1]
    s = mixed[1] + mixed[2]
    z = longs[0]
    println("derived ${k} ${s} ${z}")
    uint32 u = 4000000000
    us = [ u, 0 ]
    uint16 h = 65000
    hs = [ h, 1 ]
    println("narrow ${us[0]} ${hs[0]}")
    i = 0
    total = 0.0
    while i < 3 {
        total = total + rolls[i]
        i = i + 1
    }
    println("total ${total}")
}
AE
want='rolls 0.565487 0.785398 1.00531
mixed 1 2.5 3
calls 0.5 1.5
ints 1 3
longs 4000000000 1
derived 2.5 5.5 4000000000
narrow 4000000000 65000
total 2.35619'
got="$(cd "$tmp" && AETHER_HOME="$ROOT" "$AE" run main.ae 2>&1)"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] array_literal_element_type: output differs"
    printf '%s\n' "$got" | head -8 | sed 's/^/        /'
    fail=1
fi

expect_error() {  # file, pattern, label
    out="$(AETHER_HOME="$ROOT" "$AETHERC" "$1" "$tmp/out.c" 2>&1)"
    if ! printf '%s' "$out" | grep -q "$2"; then
        echo "  [FAIL] array_literal_element_type: $3"
        printf '%s\n' "$out" | grep -E "^error|-->" | head -4 | sed 's/^/        /'
        fail=1
    fi
}

cat > "$tmp/narrow_var.ae" <<'AE'
main() {
    r = 0
    r = 2.5
    println("${r}")
}
AE
expect_error "$tmp/narrow_var.ae" "narrowing assignment to 'r': its type was inferred as int from its initializer, but a float is assigned" "a float into an inferred int variable was not refused"

cat > "$tmp/narrow_elem.ae" <<'AE'
main() {
    xs = [ 1, 2 ]
    xs[0] = 7.5
    println("${xs[0]}")
}
AE
expect_error "$tmp/narrow_elem.ae" "narrowing assignment to an element of 'xs'" "a float into an inferred int array element was not refused"

# An int array that came from a call, not a literal, is not "inferred from
# the array literal": it narrows like any other int slot, without the
# literal-shaped advice.
cat > "$tmp/from_call.ae" <<'AE'
mk() -> int[] {
    int[2] r = [ 1, 2 ]
    return r
}
main() {
    ys = mk()
    ys[0] = 7.5
    println("${ys[0]}")
}
AE
out="$(AETHER_HOME="$ROOT" "$AETHERC" "$tmp/from_call.ae" "$tmp/out.c" 2>&1)"
if printf '%s' "$out" | grep -q "from the array literal"; then
    echo "  [FAIL] array_literal_element_type: an int array bound from a call got the array-literal narrowing message"
    printf '%s\n' "$out" | grep -E "^error|-->" | head -4 | sed 's/^/        /'
    fail=1
fi

cat > "$tmp/nested.ae" <<'AE'
main() {
    grid = [ [ 1.0, 2.0 ], [ 3.0, 4.0 ] ]
    println("${grid[0][0]}")
}
AE
expect_error "$tmp/nested.ae" "an array literal cannot hold an array literal" "a nested array literal was not refused"

# The explicit forms stay legal: an annotated int slot narrows on purpose.
cat > "$tmp/explicit.ae" <<'AE'
main() {
    int k = 4.5
    int[2] xs = [ 1, 2 ]
    xs[0] = 7.5
    println("${k} ${xs[0]}")
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/explicit.ae" 2>&1 | tail -1)"
if [ "$got" != "4 7" ]; then
    echo "  [FAIL] array_literal_element_type: the annotated narrowing forms no longer compile (got '$got')"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] array_literal_element_type: literals join their element types; inferred-int narrowing and nested literals are refused"
fi
exit $fail
