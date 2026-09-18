#!/bin/sh
# std.snapshot CONCURRENT copy-on-write test (issue #840 — concurrency coverage).
#
# The snapshot_cow regression test covers the store/cas/load/reclaim sequence but
# SINGLE-THREADED, and the #841 concurrent-cache benchmark's COW design uses
# store() (unconditional overwrite), not a CAS loop. So nothing else in-tree
# proves the property a real many-writer user of std.snapshot depends on: a CAS
# retry loop over a snapshot cell must NEVER LOSE an update when many threads
# publish concurrently. This drives std.worker (a real thread pool) at the cell:
#
#   1. 500 concurrent CAS increments -> the cell's value must be EXACTLY 500
#      (a broken cas/loop loses updates here).
#   2. 300 writers + 300 concurrent readers -> writers still reach 300 and every
#      lock-free read returns a real published value (no torn reads).
#
# The .ae driver self-reports "snapshot_concurrent: N passing, 0 failing" on its
# last line; this wrapper asserts on it. Skips (green) with no threads / no ae.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] snapshot_concurrent: ae not built"
    exit 0
fi

cd "$ROOT" || exit 1

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/probe.ae" >"$TMPDIR/out.log" 2>&1; then
    echo "  [FAIL] snapshot_concurrent: ae run exited non-zero"
    tail -40 "$TMPDIR/out.log" | sed 's/^/    /'
    exit 1
fi

if ! grep -q "snapshot_concurrent: 2 passing, 0 failing" "$TMPDIR/out.log"; then
    echo "  [FAIL] snapshot_concurrent - not all cases passed"
    tail -40 "$TMPDIR/out.log" | sed 's/^/    /'
    exit 1
fi

echo "  [PASS] snapshot_concurrent (issue #840 — concurrent CAS, no lost updates)"
exit 0
