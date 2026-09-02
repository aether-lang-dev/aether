#!/bin/sh
# Integration test for the string_new_with_length heap-source fix: a
# function that builds a tuple return value with string_new_with_length
# is heap-returning at that position, and its result is freed at every
# call site. The probe runs 8000 interleaved success/error encode
# cycles and bounds RSS growth — pre-fix the success path leaked the
# constructor's buffer every iteration (~336 KB/commit in avn).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] string_new_with_length_no_leak: ae not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

if ! "$AE" build probe.ae -o "$tmpdir/probe" >"$tmpdir/build.log" 2>&1; then
    echo "  [FAIL] string_new_with_length_no_leak: build failed"
    sed 's/^/    /' "$tmpdir/build.log" | head -30
    exit 1
fi

if ! "$tmpdir/probe" >"$tmpdir/run.log" 2>&1; then
    echo "  [FAIL] string_new_with_length_no_leak: probe exited non-zero"
    sed 's/^/    /' "$tmpdir/run.log" | head -20
    exit 1
fi

if ! grep -q "all cases passed" "$tmpdir/run.log"; then
    echo "  [FAIL] string_new_with_length_no_leak: probe output missing success marker"
    sed 's/^/    /' "$tmpdir/run.log" | head -20
    exit 1
fi

echo "  [PASS] string_new_with_length_no_leak: 8000 heap-returning cycles, RSS bounded"
exit 0
