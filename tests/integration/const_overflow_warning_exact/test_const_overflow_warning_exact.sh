#!/bin/sh
# W1003 (int constant expression overflows 32 bits) quotes the exact value
# and the value the runtime wraps to. The folder used to compute both in a
# double, so past 2^53 both numbers were wrong: `1000000007 * 1000000007`
# was reported as 1000000014000000000 wrapping to -371520512, when the
# product is 1000000014000000049 and wraps to -371520463. The warning is
# the compiler proving an overflow, so the numbers it proves must be right.
# A `long` expression that overflows 64 bits gets its own warning, and one
# that fits gets none.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AETHERC" ] && [ ! -x "$AETHERC.exe" ]; then
    echo "  [SKIP] const_overflow_warning_exact: $AETHERC not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fail=0

cat > "$tmp/int.ae" <<'AE'
main() { println("${1000000007 * 1000000007}") }
AE
out="$("$AETHERC" "$tmp/int.ae" "$tmp/int.c" 2>&1)"
if ! printf '%s' "$out" | grep -q "exact value is 1000000014000000049, which wraps to -371520463"; then
    echo "  [FAIL] const_overflow_warning_exact: W1003 does not quote the exact product and its wrap"
    printf '%s\n' "$out" | grep W1003 | sed 's/^/        /'
    fail=1
fi

cat > "$tmp/long.ae" <<'AE'
main() { println("${9223372036854775807 + 1}") }
AE
out="$("$AETHERC" "$tmp/long.ae" "$tmp/long.c" 2>&1)"
if ! printf '%s' "$out" | grep -q "long constant expression overflows 64 bits and wraps to -9223372036854775808"; then
    echo "  [FAIL] const_overflow_warning_exact: a long expression past 64 bits is not warned about"
    printf '%s\n' "$out" | head -3 | sed 's/^/        /'
    fail=1
fi

cat > "$tmp/fits.ae" <<'AE'
main() { println("${4611686018427387904 + 4611686018427387903}") }
AE
out="$("$AETHERC" "$tmp/fits.ae" "$tmp/fits.c" 2>&1)"
if printf '%s' "$out" | grep -q "W1003"; then
    echo "  [FAIL] const_overflow_warning_exact: a long expression that fits was warned about"
    printf '%s\n' "$out" | grep W1003 | sed 's/^/        /'
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] const_overflow_warning_exact: W1003 quotes exact values; long overflow warned, long fit silent"
fi
exit $fail
