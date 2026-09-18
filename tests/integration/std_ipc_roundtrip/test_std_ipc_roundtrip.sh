#!/bin/sh
# std.ipc round-trip integration test.
#
# Exercises the parent ↔ child back-channel via the three spawn
# variants (run_pipe_drain_and_wait, run_pipe + wait_pid, and
# run_capture as a negative control). Probe.ae compiles the
# child binary in /tmp and runs all three cases.
#
# Skips on Windows — std.ipc is POSIX-only in v1; Windows
# parent_channel returns -1 unconditionally and the parent-side
# spawn returns "unsupported on Windows".

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ "$OS" = "Windows_NT" ]; then
    echo "  [SKIP] std_ipc_roundtrip: std.ipc is POSIX-only in v1"
    exit 0
fi

if [ ! -x "$AE" ]; then
    echo "  [SKIP] std_ipc_roundtrip: ae not built"
    exit 0
fi

cd "$ROOT" || exit 1

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true; rm -f /tmp/std_ipc_roundtrip_child || true' EXIT

if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/probe.ae" >"$TMPDIR/out.log" 2>&1; then
    echo "  [FAIL] std_ipc_roundtrip"
    tail -40 "$TMPDIR/out.log" | sed 's/^/    /'
    exit 1
fi

if ! grep -q "std_ipc_roundtrip: 3 passing, 0 failing" "$TMPDIR/out.log"; then
    echo "  [FAIL] std_ipc_roundtrip — not all 3 cases passed"
    tail -40 "$TMPDIR/out.log" | sed 's/^/    /'
    exit 1
fi

echo "  [PASS] std_ipc_roundtrip"
exit 0
