#!/bin/sh
# Regression (#3843): `for x in xs` counts in 64 bits.
#
# The loop is lowered to an index loop over `xs.len`, which is an int, so a
# slice of more than 2^31 elements ran the wrong number of times. Its own
# length and index are now int64_t (the parser's `len64`), and a user's
# `.len` is the checked aether_slice_len_int. A run over 2^31 elements would
# need gigabytes, so the generated C is what is checked.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
EXE=""
[ -x "$ROOT/build/aetherc" ] || EXE=".exe"
AETHERC="$ROOT/build/aetherc$EXE"
if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] for_in_wide_count: compiler not built"
    exit 0
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/loop.ae" <<'AE'
main() {
    xs = make([]int, 4)
    t = 0
    for x in xs {
        t = t + x
    }
    for i in 0..xs.len {
        t = t + xs[i]
    }
    println("${t}")
}
AE
if ! AETHER_HOME="" "$AETHERC" "$TMP/loop.ae" "$TMP/loop.c" > "$TMP/gen.log" 2>&1; then
    echo "  [FAIL] for_in_wide_count: aetherc failed"
    sed 's/^/        /' "$TMP/gen.log" | head -10
    exit 1
fi
fail=0
if ! grep -q "int64_t _fi_n_[0-9]* = (aether_slice_len(" "$TMP/loop.c"; then
    echo "  [FAIL] for_in_wide_count: the for-in length is not the 64-bit aether_slice_len"
    grep -m2 "_fi_n_" "$TMP/loop.c" | sed 's/^/        /'
    fail=1
fi
if ! grep -q "int64_t _fi_i_[0-9]* = 0" "$TMP/loop.c"; then
    echo "  [FAIL] for_in_wide_count: the for-in index is not int64_t"
    grep -m2 "_fi_i_" "$TMP/loop.c" | sed 's/^/        /'
    fail=1
fi
if grep -q "(int)aether_slice_len(" "$TMP/loop.c" || ! grep -q "aether_slice_len_int(xs)" "$TMP/loop.c"; then
    echo "  [FAIL] for_in_wide_count: a user's .len is not the checked aether_slice_len_int"
    grep -m2 "aether_slice_len" "$TMP/loop.c" | sed 's/^/        /'
    fail=1
fi
[ "$fail" -eq 0 ] || exit 1
echo "  [PASS] for_in_wide_count: for x in xs counts in 64 bits; .len is checked"
exit 0
