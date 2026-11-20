#!/bin/sh
# A string passed where a parameter takes a number used to pass the front
# end and reach the C compiler ("incompatible pointer to integer
# conversion"): `string.pad_start(s, 8, "0")`, whose pad is a character code
# (`int`), and an Aether function's own `int` parameter alike. The front end
# now refuses it at the call; for a one-character literal the hint gives the
# character's code. The documented call, with the code, compiles and runs,
# and a string still goes to a `ptr` parameter.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AETHERC" ] && [ ! -x "$AETHERC.exe" ]; then
    echo "  [SKIP] string_for_number_argument_reject: $AETHERC not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fail=0

cat > "$tmp/bad.ae" <<'AE'
import std.string

takes_int(n: int) -> int { return n }
takes_float(x: float) -> float { return x }

main() {
    println(string.pad_start("7", 8, "0"))
    println(string.pad_end("7", 8, "ab"))
    s = "12"
    println("${takes_int(s)}")
    println("${takes_float("1.5")}")
}
AE
out="$(AETHER_HOME="$ROOT" "$AETHERC" "$tmp/bad.ae" "$tmp/bad.c" 2>&1)"
expect() {
    if ! printf '%s' "$out" | grep -qF "$1"; then
        echo "  [FAIL] string_for_number_argument_reject: $2"
        printf '%s\n' "$out" | grep -E "error|help" | head -8 | sed 's/^/        /'
        fail=1
    fi
}
expect "Argument 3 'pad_char' of 'string.pad_start': expected int, got string" \
    "a one-character string to pad_start was not refused"
expect 'pass 48 for "0"' "the hint does not give the character code"
expect "Argument 3 'pad_char' of 'string.pad_end': expected int, got string" \
    "a string to pad_end was not refused"
expect "Argument 1 'n' of 'takes_int': expected int, got string" \
    "a string variable to a user function's int was not refused"
expect "Argument 1 'x' of 'takes_float': expected float, got string" \
    "a string to a float parameter was not refused"
if printf '%s' "$out" | grep -q "incompatible pointer to integer"; then
    echo "  [FAIL] string_for_number_argument_reject: the C compiler was reached"
    fail=1
fi

cat > "$tmp/good.ae" <<'AE'
import std.string

takes_ptr(p: ptr) -> int {
    if p == null { return 0 }
    return 1
}

main() {
    println(string.pad_start("7", 8, 48))
    println("[${string.pad_end("ab", 4, 32)}]")
    println("${takes_ptr("x")}")
}
AE
if ! AETHER_HOME="$ROOT" "$AE" build "$tmp/good.ae" -o "$tmp/good" >"$tmp/good.log" 2>&1; then
    echo "  [FAIL] string_for_number_argument_reject: the documented calls do not build"
    sed 's/^/        /' "$tmp/good.log" | head -10
    fail=1
else
    got="$("$tmp/good" | tr '\n' ' ')"
    if [ "$got" != "00000007 [ab  ] 1 " ]; then
        echo "  [FAIL] string_for_number_argument_reject: the documented calls printed '$got'"
        fail=1
    fi
fi

[ "$fail" -eq 0 ] || exit 1
echo "  [PASS] string_for_number_argument_reject"
