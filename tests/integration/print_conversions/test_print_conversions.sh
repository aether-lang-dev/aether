#!/bin/sh
# #3527: every kind print, println and an interpolation write has its own
# conversion, and the int one is used only for the kinds that are ints on
# purpose (int, byte, uint8, uint16, an enum).
#
# Each print path ended its chain of kinds with `%d`, so a kind a chain had
# not learned was read as an int: `println(x)` of a long double printed
# garbage, `print(u)` of a uint64 printed -1, and so did a uint64 in a
# literal print format. Anything with no conversion is now a compile error
# naming its type (tests/compiler/test_codegen.c covers that).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=print_conversions

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] $NAME: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT

cat > "$tmp/main.ae" <<'AE'
enum Color { Red, Green }

show(v: longdouble) -> int {
    println(v)
    return 0
}

main() {
    longdouble x = 2.5
    println(x)
    print(x)
    print("\n")
    show(x)
    uint64 u = 18446744073709551615
    print(u)
    print("\n")
    println(u)
    print("%d and %d\n", u, x)
    println("%d", u)
    byte b = 200
    uint16 h = 65000
    c = Color.Green
    print(b)
    print(" ")
    print(h)
    print(" ")
    println(c)
    println("${x} ${u} ${b} ${h} ${c}")
}
AE
want='2.500000
2.500000
2.500000
18446744073709551615
18446744073709551615
18446744073709551615 and 2.500000
18446744073709551615
200 65000 1
2.5 18446744073709551615 200 65000 1'
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/main.ae" 2>/dev/null | tr -d '\r')"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] $NAME: output differs"
    printf '%s\n' "$got" | head -12 | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] $NAME: long doubles, uint64s and the int kinds print with their own conversions"
exit 0
