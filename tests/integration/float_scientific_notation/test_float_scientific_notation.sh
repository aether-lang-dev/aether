#!/bin/sh
# Float literals in scientific notation are one token (#1954).
#
# The probe asserts the values, and this asserts the two shapes that made the
# bug hard to see: the compile is CLEAN (the misparse used to leave a
# spurious "unused variable 'println'" behind, because the call on the next
# line was swallowed into the expression), and a binary is actually produced
# (the misparse ended compilation without one).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] float_scientific_notation: build/ae missing"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMP/probe" > "$TMP/build.log" 2>&1; then
    echo "  [FAIL] float_scientific_notation: build failed"
    sed 's/^/        /' "$TMP/build.log" | head -15
    exit 1
fi

# A binary, not just a clean exit. The misparse ended compilation with no
# artifact while reporting only a warning.
[ -x "$TMP/probe" ] || { echo "  [FAIL] float_scientific_notation: no binary produced"; exit 1; }

# No warnings. `println` reported as an unused VARIABLE is the fingerprint of
# the statement being swallowed into the line above.
if grep -q "^warning" "$TMP/build.log"; then
    echo "  [FAIL] float_scientific_notation: the build warned"
    grep -A2 "^warning" "$TMP/build.log" | sed 's/^/        /' | head -8
    exit 1
fi

if ! "$TMP/probe" > "$TMP/run.out" 2>&1; then
    echo "  [FAIL] float_scientific_notation: probe reported a failure"
    sed 's/^/        /' "$TMP/run.out" | head -10
    exit 1
fi
grep -q "All scientific-notation cases pass" "$TMP/run.out" || {
    echo "  [FAIL] float_scientific_notation: probe did not reach the end"
    sed 's/^/        /' "$TMP/run.out" | head -10
    exit 1
}

echo "  [PASS] float_scientific_notation: every exponent spelling lexes as one number"
exit 0
