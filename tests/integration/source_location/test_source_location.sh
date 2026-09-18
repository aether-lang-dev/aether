#!/bin/sh
# Issue #265 regression: source-location intrinsics __LINE__/__FILE__/__func__.
#
# Verifies:
#   - __LINE__ returns DIFFERENT integer values for distinct call sites.
#   - __FILE__ returns the path to the .ae source file.
#   - __func__ returns the enclosing function name.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/uses_intrinsics.ae" >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run exited non-zero"
    cat "$TMPDIR/err.log" | head -10
    exit 1
fi

# Expected lines (lines refer to where the report() calls happen in
# uses_intrinsics.ae): main's report on line 26, run()'s on lines 21
# and 22. File path is .../uses_intrinsics.ae. Functions are main / run.
EXPECTED='m L26 F=.*uses_intrinsics.ae fn=main
a L21 F=.*uses_intrinsics.ae fn=run
b L22 F=.*uses_intrinsics.ae fn=run'

# Verify each line matches its pattern (use case glob line by line so
# the file-path suffix is wildcarded — different test runs may live
# in different absolute paths).
LINE1=$(sed -n '1p' "$ACTUAL")
LINE2=$(sed -n '2p' "$ACTUAL")
LINE3=$(sed -n '3p' "$ACTUAL")

case "$LINE1" in
    "m L26 F="*"uses_intrinsics.ae fn=main") ;;
    *) echo "  [FAIL] line 1: '$LINE1'"; exit 1 ;;
esac
case "$LINE2" in
    "a L21 F="*"uses_intrinsics.ae fn=run") ;;
    *) echo "  [FAIL] line 2: '$LINE2'"; exit 1 ;;
esac
case "$LINE3" in
    "b L22 F="*"uses_intrinsics.ae fn=run") ;;
    *) echo "  [FAIL] line 3: '$LINE3'"; exit 1 ;;
esac

# Also verify __LINE__ values differ across the three sites.
L1_NUM=$(echo "$LINE1" | sed -E 's/.* L([0-9]+) .*/\1/')
L2_NUM=$(echo "$LINE2" | sed -E 's/.* L([0-9]+) .*/\1/')
L3_NUM=$(echo "$LINE3" | sed -E 's/.* L([0-9]+) .*/\1/')
if [ "$L1_NUM" = "$L2_NUM" ] || [ "$L2_NUM" = "$L3_NUM" ]; then
    echo "  [FAIL] __LINE__ should yield distinct values at distinct sites: $L1_NUM $L2_NUM $L3_NUM"
    exit 1
fi

echo "  [PASS] source_location intrinsics: __LINE__/__FILE__/__func__ expand correctly"
