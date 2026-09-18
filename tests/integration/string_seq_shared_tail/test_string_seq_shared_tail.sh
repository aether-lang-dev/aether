#!/bin/sh
# Integration: structural sharing of a tail across two cons lists.
#
# Build a shared tail [y, z] held by two parents `a = [a|y,z]` and
# `b = [b|y,z]` (each retains the shared tail explicitly via
# string.seq_retain). Drop our local handle to the shared tail. Free
# `a`. Confirm `b` is still walkable. Free `b`. Print "ok".
#
# This validates that string.seq_free's refcount path correctly
# stops the iterative spine walk at the first cell whose refcount
# stays > 0 after decrement, leaving the rest of the chain intact
# for the other owner.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/repro.ae" >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run exited non-zero"
    head -30 "$TMPDIR/err.log"
    cat "$ACTUAL"
    exit 1
fi

if ! grep -qx 'ok' "$ACTUAL"; then
    echo "  [FAIL] expected 'ok' in stdout"
    echo "--- stdout ---"
    cat "$ACTUAL"
    echo "--- stderr ---"
    head -20 "$TMPDIR/err.log"
    exit 1
fi

echo "  [PASS] string_seq_shared_tail: refcounted structural sharing across two cons lists"
