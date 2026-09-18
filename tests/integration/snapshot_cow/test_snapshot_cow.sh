#!/bin/sh
# std.snapshot copy-on-write cell regression test (issue #840).
#
# std.snapshot is a lock-free read / rare-write snapshot cell for
# read-mostly shared data (config, routing tables, feature flags). The
# cell holds one atomic pointer to an immutable value; readers acquire-
# load it lock-free, the writer publishes via store / cas and gets the
# displaced value back to reclaim after a grace period.
#
# Exercises 7 cases:
#   1. new / load round-trip
#   2. store publishes new + returns displaced old; load sees new
#   3. cas success swaps when expected matches
#   4. cas failure leaves the cell unchanged on stale expected
#   5. writer-side deferred-reclamation CAS loop (free N-1 at N+1)
#   6. null-cell defence on load / store / cas / free
#   7. new(null) is a legal empty cell
#
# The .ae driver self-reports "snapshot_cow: N passing, 0 failing" on
# its last line; this wrapper asserts on it.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] snapshot_cow: ae not built"
    exit 0
fi

cd "$ROOT" || exit 1

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/probe.ae" >"$TMPDIR/out.log" 2>&1; then
    echo "  [FAIL] snapshot_cow: ae run exited non-zero"
    tail -40 "$TMPDIR/out.log" | sed 's/^/    /'
    exit 1
fi

if ! grep -q "snapshot_cow: 7 passing, 0 failing" "$TMPDIR/out.log"; then
    echo "  [FAIL] snapshot_cow - not all 7 cases passed"
    tail -40 "$TMPDIR/out.log" | sed 's/^/    /'
    exit 1
fi

echo "  [PASS] snapshot_cow (issue #840)"
exit 0
