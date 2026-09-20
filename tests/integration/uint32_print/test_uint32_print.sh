#!/bin/sh
# A uint32 value prints as itself, not as a negative int.
#
# Every print path chose `%d` for TYPE_UINT32 — the bare `print(x)` /
# `println(x)` statement and expression forms, the `print("...%d", x)`
# format-string form, and `${x}` interpolation (printf mode and
# `_aether_interp`) — so `uint32 x = 4000000000` came out as -294967296
# everywhere. uint32_t is unsigned int on every supported target; `%u`
# is its conversion.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] uint32_print: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT

cat > "$tmp/main.ae" <<'AE'
show(v: uint32) {
    print(v)
    print(" ")
    println(v)
}

main() {
    uint32 x = 4000000000
    show(x)
    println("${x}")
    s = "v=${x}"
    println(s)
    print("%u\n", x)
    uint32[2] xs = [ x, 7 ]
    println("${xs[0]} ${xs[1]}")
    uint16 h = 65000
    byte b = 200
    println("${h} ${b}")
}
AE
want='4000000000 4000000000
4000000000
v=4000000000
4000000000
4000000000 7
65000 200'
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/main.ae" 2>&1)"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] uint32_print: output differs"
    printf '%s\n' "$got" | head -8 | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] uint32_print: uint32 values print unsigned through every print path"
exit 0
