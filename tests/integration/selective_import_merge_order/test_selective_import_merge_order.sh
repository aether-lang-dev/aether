#!/bin/sh
# Regression: aether#1009 — selective import in the consumer must not break a
# dependency's qualified `os.*` surface when std.os isn't the dep's first import.
#
# dep/module.ae: import std.string; import std.os  (os SECOND) + os.now_monotonic_ns()
# main.ae:       import dep; import std.os (getenv)  (getenv never called)
# Before the fix: E0301 Undefined function 'os.now_monotonic_ns'.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

# Run from the test dir so `import dep` resolves to ./dep/module.ae.
cd "$SCRIPT_DIR"

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run main.ae >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run main.ae exited non-zero — aether#1009 selective-import merge order"
    cat "$TMPDIR/err.log" | head -10
    exit 1
fi

if grep -q '^OK ' "$ACTUAL"; then
    echo "  [PASS] selective_import_merge_order: qualified os.* in dep resolves under a consumer selective import"
    exit 0
fi

echo "  [FAIL] expected 'OK <ns>', got:"
cat "$ACTUAL" | head -5
exit 1
