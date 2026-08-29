#!/bin/sh
# Regression for #1781: a boolean condition wrapped so the continuation line
# begins with `||` used to be misparsed as closure parameters, and the error
# ("Expected '->' or '{' after closure parameters") pointed at closure syntax
# far from the real cause.
#
# Aether continues an expression only when the operator sits at the END of the
# previous line (operator_starts_newline); a leading operator starts a fresh
# expression. That design is intentional, so the fix is a targeted diagnostic:
#   (1) leading `||` on a continuation line -> an error that NAMES the
#       wrapped-condition cause and the fix (operator at end of prev line, or
#       parenthesise);
#   (2) the recommended `||`-at-end-of-line form compiles cleanly.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
AE="$ROOT/build/ae"

fail=0

# (1) The bad case must error, and the error must explain the real cause.
tmpdir="$(mktemp -d)"
log="$tmpdir/cc.log"
"$AETHERC" "$SCRIPT_DIR/leading_or_condition.ae" "$tmpdir/out.c" >"$log" 2>&1
if ! grep -q '^error' "$log"; then
    echo "  [FAIL] leading-|| condition reported no error"
    fail=1
elif ! grep -qi "closure parameters" "$log" || ! grep -qi "previous line\|parenthesise\|wrapped condition" "$log"; then
    echo "  [FAIL] leading-|| error doesn't explain the wrapped-condition cause + fix"
    echo "        got:"; sed 's/^/          /' "$log" | head -4
    fail=1
else
    echo "  [PASS] leading-|| condition gives the wrapped-condition diagnostic"
fi
rm -rf "$tmpdir"

# (2) The recommended form (operator at end of previous line) must compile+run.
out="$("$AE" run "$SCRIPT_DIR/trailing_or_condition.ae" 2>&1)"
if [ "$out" = "yes" ]; then
    echo "  [PASS] trailing-|| continuation compiles and runs"
else
    echo "  [FAIL] trailing-|| continuation: got '$out', expected 'yes'"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "PASS: #1781 condition-continuation diagnostic"
    exit 0
fi
echo "FAIL: #1781"
exit 1
