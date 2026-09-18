#!/bin/sh
# Issue #333 regression: DSL block receiver scoping.
#
# Inside a trailing closure bound to a member-access call, bare-name
# function references should fall back through the receiver's
# namespace (or struct type) before erroring. Removes the
# `import X (a, b, c)` boilerplate that otherwise grows with every
# new SDK setter.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

cd "$SCRIPT_DIR"

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run main.ae >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run main.ae exited non-zero — DSL receiver fallback broken"
    head -20 "$TMPDIR/err.log"
    exit 1
fi

EXPECTED1="run: label=test1 script=a.sh tag=smoke"
EXPECTED2="run: label=test2 script=b.sh tag=integration"

if ! grep -qF "$EXPECTED1" "$ACTUAL"; then
    echo "  [FAIL] missing expected line: '$EXPECTED1'"
    echo "--- actual:"
    cat "$ACTUAL"
    exit 1
fi
if ! grep -qF "$EXPECTED2" "$ACTUAL"; then
    echo "  [FAIL] missing expected line: '$EXPECTED2'"
    echo "--- actual:"
    cat "$ACTUAL"
    exit 1
fi

echo "  [PASS] dsl_receiver_scoping: bare-name fallback + per-call stamping"
