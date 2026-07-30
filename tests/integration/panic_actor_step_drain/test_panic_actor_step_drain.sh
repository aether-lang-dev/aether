#!/bin/sh
# Regression: #1301 scheduler-barrier drain. A panicking actor's
# step allocations are freed by the unwind journal; the process keeps
# running (actor marked dead, remaining messages dropped). CI's
# LeakSanitizer jobs verify the drain by running this binary.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] panic_actor_step_drain: ae not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

if ! "$AE" build probe.ae -o "$tmpdir/probe" >"$tmpdir/build.log" 2>&1; then
    echo "  [FAIL] panic_actor_step_drain: build failed"
    sed 's/^/    /' "$tmpdir/build.log" | head -20
    exit 1
fi

if ! AETHER_NO_INLINE=1 "$tmpdir/probe" >"$tmpdir/run.log" 2>&1; then
    echo "  [FAIL] panic_actor_step_drain: probe exited non-zero"
    sed 's/^/    /' "$tmpdir/run.log" | head -20
    exit 1
fi

if ! grep -q "panicking actor drained" "$tmpdir/run.log"; then
    echo "  [FAIL] panic_actor_step_drain: missing success marker"
    sed 's/^/    /' "$tmpdir/run.log" | head -20
    exit 1
fi

echo "  [PASS] panic_actor_step_drain: step-scoped allocations drained on actor panic"
exit 0
