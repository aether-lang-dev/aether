#!/bin/sh
# Issue #333 nested DSL blocks: inner block's receiver wins for
# names defined on it; outer's receiver applies to outer-block-local
# names. Both blocks correctly stamp their receiver on their
# respective closure scopes.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

cd "$SCRIPT_DIR"

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run main.ae >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run main.ae exited non-zero"
    head -20 "$TMPDIR/err.log"
    exit 1
fi

# Inner runs first because builder bodies execute after their
# trailing-block setters populate the builder map; we just check
# both expected lines are present.
for line in \
    "inner: label=i-label note=inner-note" \
    "outer: label=o-label mark=outer-mark"
do
    if ! grep -qF "$line" "$ACTUAL"; then
        echo "  [FAIL] missing expected line: '$line'"
        echo "--- actual:"; cat "$ACTUAL"; exit 1
    fi
done

echo "  [PASS] dsl_receiver_scoping_nested: inner+outer receiver stamping"
