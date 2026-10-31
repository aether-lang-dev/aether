#!/bin/sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "  [SKIP] std_ipc_socket: std.ipc socket channel is POSIX-only in v1"
        exit 0 ;;
esac

if [ ! -x "$AE" ]; then
    echo "  [SKIP] std_ipc_socket: ae not built"
    exit 0
fi

cd "$ROOT" || exit 1

TMPDIR="$(mktemp -d)"
export TMPDIR
trap 'rm -rf "$TMPDIR" || true' EXIT

if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/probe.ae" >"$TMPDIR/out.log" 2>&1; then
    echo "  [FAIL] std_ipc_socket"
    tail -40 "$TMPDIR/out.log" | sed 's/^/    /'
    exit 1
fi

echo "  [PASS] std_ipc_socket"
