#!/bin/sh
# Regression: sharded actor map (lock striping via N owner actors).
#
# Builds a sharded map from N=4 shard actors, routes each key to
# `hash(key) % N`, sets/gets several keys across shards, deletes one, and
# asserts correctness — all on top of the existing `?` ask + `reply`
# primitive. The program exits non-zero (and prints FAIL lines) on any
# routing or value mismatch.
#
# Pass: program exits 0 and prints the PASS summary spanning >1 shard.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

ACTUAL="$TMPDIR/actual.txt"
if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/sharded_map.ae" >"$ACTUAL" 2>"$TMPDIR/err.log"; then
    echo "  [FAIL] ae run exited non-zero — sharded map routing/correctness regression?"
    echo "--- stdout ---"; cat "$ACTUAL"
    echo "--- stderr ---"; head -30 "$TMPDIR/err.log"
    exit 1
fi

if grep -q '^FAIL' "$ACTUAL"; then
    echo "  [FAIL] sharded map reported a mismatch"
    echo "--- stdout ---"; cat "$ACTUAL"
    exit 1
fi

if ! grep -q '^PASS: sharded map correct across' "$ACTUAL"; then
    echo "  [FAIL] expected PASS summary line not found"
    echo "--- stdout ---"; cat "$ACTUAL"
    exit 1
fi

echo "  [PASS] sharded_actor_map: keys route across shards, get/set/delete correct"
