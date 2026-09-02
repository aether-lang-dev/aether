#!/bin/sh
# Integration test for the string_substring_n heap-source fix: a value
# built by string_substring_n is freed at every call site, whether a
# direct local or returned through a wrapper. The probe runs 8000
# iterations of both shapes and bounds RSS growth — pre-fix each
# iteration leaked the minted buffer (avn's residual ~77 KB/commit).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] string_substring_n_no_leak: ae not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

if ! "$AE" build probe.ae -o "$tmpdir/probe" >"$tmpdir/build.log" 2>&1; then
    echo "  [FAIL] string_substring_n_no_leak: build failed"
    sed 's/^/    /' "$tmpdir/build.log" | head -30
    exit 1
fi

if ! "$tmpdir/probe" >"$tmpdir/run.log" 2>&1; then
    echo "  [FAIL] string_substring_n_no_leak: probe exited non-zero"
    sed 's/^/    /' "$tmpdir/run.log" | head -20
    exit 1
fi

if ! grep -q "all cases passed" "$tmpdir/run.log"; then
    echo "  [FAIL] string_substring_n_no_leak: probe output missing success marker"
    sed 's/^/    /' "$tmpdir/run.log" | head -20
    exit 1
fi

echo "  [PASS] string_substring_n_no_leak: 8000 substring_n cycles, RSS bounded"
exit 0
