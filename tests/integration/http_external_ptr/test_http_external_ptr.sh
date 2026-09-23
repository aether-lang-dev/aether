#!/bin/sh
# Regression: std.http request/response accessors work on externally-
# constructed HttpRequest / HttpServerResponse pointers. See probe.ae
# for the test matrix.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

# This used to skip on Windows, with no reason recorded, and it passes
# there. It is also what guards the ownership convention #1739's header
# reuse had to preserve -- each header string is its own allocation,
# freed one by one by C like shim.c -- so it runs everywhere now.

TMPDIR="$(mktemp -d)"; trap 'rm -rf "$TMPDIR"' EXIT

if ! "$ROOT/build/ae" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" \
        --extra "$SCRIPT_DIR/shim.c" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] http_external_ptr: build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -15
    exit 1
fi

if ! "$TMPDIR/probe" >"$TMPDIR/run.log" 2>&1; then
    echo "  [FAIL] http_external_ptr: probe exited non-zero"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

if ! grep -q "All external-ptr interop tests passed" "$TMPDIR/run.log"; then
    echo "  [FAIL] http_external_ptr: didn't reach the final PASS line"
    sed 's/^/    /' "$TMPDIR/run.log" | head -30
    exit 1
fi

echo "  [PASS] http_external_ptr: 9 cases"
