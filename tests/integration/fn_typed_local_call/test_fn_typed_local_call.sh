#!/bin/sh
# Phase A3: direct invocation of function-typed locals (fn(args)
# syntax). Foundation for #260 D pure-Aether middleware composition.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/uses_fn_call.ae" >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run exited non-zero"
    cat "$TMPDIR/err.log" | head -10
    exit 1
fi

EXPECTED='7
30
123
35
42'

if [ "$(cat "$ACTUAL")" != "$EXPECTED" ]; then
    echo "  [FAIL] output mismatch"
    echo "--- expected ---"
    echo "$EXPECTED"
    echo "--- actual ---"
    cat "$ACTUAL"
    exit 1
fi

echo "  [PASS] fn_typed_local_call: handler(args) syntax across 4 cases"
