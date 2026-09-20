#!/bin/sh
# A typed local array whose element type is spelled by an identifier —
# `uint32[2] xs = [...]`, `uint16[3] hs`, `uint8[2] bs`, `Pair[2] ps` —
# is a declaration, like `int[2] xs` is.
#
# `int`/`long`/`byte`/`uint64` are lexer keywords and reach the typed-
# declaration path through the keyword case of the statement parser;
# `uint8`/`uint16`/`uint32` and struct names are identifiers, and only the
# `IDENT IDENT` shape (`uint32 x = ...`) was recognised. `uint32[2] xs`
# parsed as an index into a variable called `uint32` and failed with
# "Undefined variable 'uint32'". The shape IDENT `[` NUMBER? `]` IDENT is
# as unambiguous as `IDENT IDENT`: an index expression statement is never
# followed by an identifier.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] ident_typed_array_decl: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

cat > "$tmp/main.ae" <<'AE'
struct Pair { a: int, b: int }

main() {
    uint32 x = 4000000000
    uint32[2] xs = [ x, 7 ]
    xs[1] = 9
    uint16[3] hs = [ 65000, 1, 2 ]
    uint8[2] bs = [ 200, 1 ]
    Pair[2] ps = [ Pair { a: 1, b: 2 }, Pair { a: 3, b: 4 } ]
    ps[1].b = 40
    println("${xs[0]} ${xs[1]} ${hs[0]} ${bs[0]} ${ps[1].b} ${ps[0].a}")
    println(xs[1])
}
AE
want='4000000000 9 65000 200 40 1
9'
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/main.ae" 2>&1)"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] ident_typed_array_decl: output differs"
    printf '%s\n' "$got" | head -8 | sed 's/^/        /'
    fail=1
fi

# The element type is pinned in the C, not widened to int.
AETHER_HOME="$ROOT" "$AETHERC" "$tmp/main.ae" "$tmp/out.c" >/dev/null 2>&1
for decl in 'uint32_t xs\[2\]' 'uint16_t hs\[3\]' 'uint8_t bs\[2\]' 'Pair ps\[2\]'; do
    if ! grep -q "$decl" "$tmp/out.c"; then
        echo "  [FAIL] ident_typed_array_decl: expected '$decl' in the generated C"
        fail=1
    fi
done

# An index expression statement keeps parsing as one.
cat > "$tmp/index.ae" <<'AE'
main() {
    int[2] xs = [ 1, 2 ]
    xs[0] = 5
    xs[1]
    println("${xs[0]}")
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/index.ae" 2>&1 | tail -1)"
if [ "$got" != "5" ]; then
    echo "  [FAIL] ident_typed_array_decl: an index expression statement no longer parses (got '$got')"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] ident_typed_array_decl: uint8/uint16/uint32 and struct-typed local arrays declare"
fi
exit $fail
