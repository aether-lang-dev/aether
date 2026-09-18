#!/bin/sh
# Issue #265 round 2: caller-site capture for source-location
# intrinsics in default arguments.
#
# Verifies:
#   - Each call to `logme(msg)` captures its own line via __LINE__.
#   - Distinct sites in the same enclosing function yield distinct lines.
#   - Explicit override at a call site wins over the default.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/uses_capture.ae" >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run exited non-zero"
    cat "$TMPDIR/err.log" | head -10
    exit 1
fi

# Expected lines reference call sites in uses_capture.ae:
#   line 16: logme("from-helper-a")
#   line 17: logme("from-helper-b")
#   line 21: logme("from-main")
#   line 23: logme("explicit-override", 999)
EXPECTED='L21: from-main
L16: from-helper-a
L17: from-helper-b
L999: explicit-override'

if [ "$(cat "$ACTUAL")" != "$EXPECTED" ]; then
    echo "  [FAIL] output mismatch"
    echo "--- expected ---"
    echo "$EXPECTED"
    echo "--- actual ---"
    cat "$ACTUAL"
    exit 1
fi

# Sanity: line numbers differ between sites.
L1=$(sed -n '1p' "$ACTUAL" | sed -E 's/^L([0-9]+):.*/\1/')
L2=$(sed -n '2p' "$ACTUAL" | sed -E 's/^L([0-9]+):.*/\1/')
L3=$(sed -n '3p' "$ACTUAL" | sed -E 's/^L([0-9]+):.*/\1/')
if [ "$L1" = "$L2" ] || [ "$L2" = "$L3" ]; then
    echo "  [FAIL] sites should yield distinct lines: $L1 $L2 $L3"
    exit 1
fi

echo "  [PASS] source_location_default_capture: __LINE__ in default args captures caller site"
