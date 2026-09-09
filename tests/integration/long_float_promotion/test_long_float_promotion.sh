#!/bin/sh
# `long` participates in float promotion the same way `int` does (#1965).
#
# The values are the assertion. The old behaviour compiled cleanly and printed
# 0 for a quotient of 0.51, so a test that only checked the build would have
# passed against the bug.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] long_float_promotion: build/ae missing"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMP/probe" > "$TMP/build.log" 2>&1; then
    echo "  [FAIL] long_float_promotion: build failed"
    sed 's/^/        /' "$TMP/build.log" | head -15
    exit 1
fi

if ! "$TMP/probe" > "$TMP/run.out" 2>&1; then
    echo "  [FAIL] long_float_promotion: probe reported a wrong value"
    sed 's/^/        /' "$TMP/run.out" | head -10
    exit 1
fi

grep -q "All long/float promotion cases pass" "$TMP/run.out" || {
    echo "  [FAIL] long_float_promotion: probe did not reach the end"
    sed 's/^/        /' "$TMP/run.out" | head -10
    exit 1
}

echo "  [PASS] long_float_promotion: long and int agree in mixed float arithmetic"
exit 0
