#!/bin/sh
# #3839: the lane block (the vector types, the std.lanes helpers and
# <emmintrin.h>) goes only into a program that uses lanes. It went into
# every program, hello included, about 4,200 preprocessed lines of it.
#
#   1. plain.ae, with no lanes, has none of it in its C.
#   2. typed.ae, which computes with f32x4, has it, builds and prints 16.
#   3. helper_only.ae names two helpers through externs of its own and
#      holds no lane type: the gate counts the extern, so the block is
#      there and the program builds and runs.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
[ -f "$AE.exe" ] && AE="$AE.exe"
[ -f "$AETHERC.exe" ] && AETHERC="$AETHERC.exe"
NAME=lane_block_gate

if [ ! -f "$AE" ] || [ ! -f "$AETHERC" ]; then
    echo "  [SKIP] $NAME: ae or aetherc not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR
export AETHER_HOME="$ROOT"
unset AE_CC
fails=0
fail() { echo "  [FAIL] $NAME: $1"; fails=$((fails + 1)); }

cp "$SCRIPT_DIR"/*.ae "$TMP/"
cd "$TMP" || exit 1

# has_block <c file>: 0 when the C carries the lane block.
has_block() {
    grep -q 'AETHER_HAS_LANES' "$1" || grep -q 'emmintrin' "$1"
}

if ! "$AETHERC" plain.ae plain.c > plain.log 2>&1; then
    sed 's/^/        /' plain.log | head -5
    fail "plain.ae did not compile"
elif has_block plain.c; then
    fail "plain.ae uses no lanes, but its C carries the lane block"
fi

for p in typed helper_only fn_type_only; do
    if ! "$AETHERC" "$p.ae" "$p.c" > "$p.log" 2>&1; then
        sed 's/^/        /' "$p.log" | head -5
        fail "$p.ae did not compile"
    elif ! has_block "$p.c"; then
        fail "$p.ae uses lanes, but its C has no lane block"
    fi
done

if ! "$AE" build typed.ae -o typed > typed_build.log 2>&1; then
    sed 's/^/        /' typed_build.log | head -5
    fail "typed.ae did not build"
else
    out="$(./typed 2>&1 | tr -d '\r')"
    [ "$out" = "16.0" ] || [ "$out" = "16" ] || fail "typed.ae printed '$out', not 16"
fi

if ! "$AE" build helper_only.ae -o helper_only > helper_build.log 2>&1; then
    grep -i -m3 "error" helper_build.log | cut -c1-200 | sed 's/^/        /'
    fail "helper_only.ae, whose externs name lane helpers, did not build"
else
    out="$(./helper_only 2>&1 | tr -d '\r')"
    [ "$out" = "lane helpers ok" ] || fail "helper_only.ae printed '$out'"
fi

if [ "$fails" -ne 0 ]; then
    exit 1
fi
echo "  [PASS] $NAME: the lane block goes only where lanes are used, an extern naming a helper included"
exit 0
