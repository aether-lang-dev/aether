#!/bin/sh
# std.sync atomic i64 CONCURRENT test (issue #2082).
#
# std.sync exposes an atomic 64-bit integer (load/store/add/sub/cas) to
# .ae — the primitive needed to build a refcount or a lock-free retire
# ring for pool-owned copy-on-write structures (std.snapshot gives the
# atomic pointer swap but not the reclamation counter). This drives a
# real thread pool (std.worker) at one atomic cell and asserts:
#
#   1. 500 concurrent atomic_add(c,1) -> the cell is EXACTLY 500 (a
#      non-atomic increment loses updates here).
#   2. a refcount released by 500 concurrent holders via atomic_sub hits
#      0 EXACTLY ONCE — the "safe to reclaim" moment fires exactly once,
#      never twice (double-free) and never zero (leak).
#   3. 500 concurrent CAS-loop increments (load -> compare_exchange -> retry)
#      land EXACTLY 500 — the only case that drives atomic_cas, exercising the
#      acquire-on-success ordering a lock/refcount winner needs.
#
# The .ae driver self-reports "sync_atomic: N passing, 0 failing" on its
# last line; this wrapper asserts on it. Skips (green) with no ae built.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] sync_atomic: ae not built"
    exit 0
fi

cd "$ROOT" || exit 1

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/probe.ae" >"$TMPDIR/out.log" 2>&1; then
    echo "  [FAIL] sync_atomic: ae run exited non-zero"
    tail -40 "$TMPDIR/out.log" | sed 's/^/    /'
    exit 1
fi

if ! grep -q "sync_atomic: 3 passing, 0 failing" "$TMPDIR/out.log"; then
    echo "  [FAIL] sync_atomic - not all cases passed"
    tail -40 "$TMPDIR/out.log" | sed 's/^/    /'
    exit 1
fi

echo "  [PASS] sync_atomic (issue #2082 — concurrent atomic add + refcount reclaim)"
exit 0
