#!/bin/sh
# Issue #333 edge cases: existing qualified calls inside the
# trailing block still work; mixing bare and qualified forms in the
# same block produces the same observable output.

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

for line in \
    "run: label=plain script=p.sh tag=plain-tag" \
    "run: label=explicit script=e.sh tag=explicit-tag" \
    "run: label=mixed script=m.sh tag=mixed-tag"
do
    if ! grep -qF "$line" "$ACTUAL"; then
        echo "  [FAIL] missing expected line: '$line'"
        echo "--- actual:"; cat "$ACTUAL"; exit 1
    fi
done

echo "  [PASS] dsl_receiver_scoping_edge: bare + qualified mixing works"
