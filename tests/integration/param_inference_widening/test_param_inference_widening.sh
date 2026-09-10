#!/bin/sh
# A parameter inferred from call sites takes the widest numeric kind supplied,
# not the first one seen (#1972).
#
# The values are the assertion. The old behaviour compiled cleanly, passed
# `ae check`, and printed the wrong number, so a test that only checked for a
# successful build would have passed against the bug.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] param_inference_widening: build/ae missing"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMP/probe" > "$TMP/build.log" 2>&1; then
    echo "  [FAIL] param_inference_widening: build failed"
    sed 's/^/        /' "$TMP/build.log" | head -15
    exit 1
fi

# The narrowing used to leak a C-level -Wliteral-conversion warning on the
# float case, pointing at generated code. Widening removes the narrowing, so
# the warning should be gone too.
if grep -q "literal-conversion" "$TMP/build.log"; then
    echo "  [FAIL] param_inference_widening: an argument is still being narrowed"
    grep -B2 -A2 "literal-conversion" "$TMP/build.log" | sed 's/^/        /' | head -10
    exit 1
fi

if ! "$TMP/probe" > "$TMP/run.out" 2>&1; then
    echo "  [FAIL] param_inference_widening: probe reported a wrong value"
    sed 's/^/        /' "$TMP/run.out" | head -10
    exit 1
fi

grep -q "All parameter-widening cases pass" "$TMP/run.out" || {
    echo "  [FAIL] param_inference_widening: probe did not reach the end"
    sed 's/^/        /' "$TMP/run.out" | head -10
    exit 1
}

echo "  [PASS] param_inference_widening: a later, wider call site wins"
exit 0
