#!/bin/sh
# std.ipc socket channel integration test: probe.ae spawns child.ae with
# os.run_socket and holds a conversation with it, then spawns
# hostile_peer.c (built here: no public API sends more than one
# descriptor) and checks recv_fd refuses it without leaking descriptors.
#
# Skips on Windows, where the socket channel is not implemented.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

# The platform from uname, never from the environment's OS, which an MSYS2
# login shell started from another shell does not carry (check_platform_env.py).
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "  [SKIP] std_ipc_socket: the std.ipc socket channel is POSIX-only"
        exit 0 ;;
esac

if [ ! -x "$AE" ]; then
    echo "  [SKIP] std_ipc_socket: ae not built"
    exit 0
fi

cd "$ROOT" || exit 1

# The probe builds its child in the temp directory under a name of its own
# process (#2771); TMPDIR names this run's directory, which the trap removes.
TMPDIR="$(mktemp -d)"
export TMPDIR
trap 'rm -rf "$TMPDIR" || true' EXIT

AETHER_IPC_HOSTILE_PEER="$TMPDIR/hostile_peer"
export AETHER_IPC_HOSTILE_PEER
if ! ${CC:-cc} -o "$AETHER_IPC_HOSTILE_PEER" "$SCRIPT_DIR/hostile_peer.c" >"$TMPDIR/cc.log" 2>&1; then
    echo "  [FAIL] std_ipc_socket: cannot build hostile_peer.c"
    sed 's/^/    /' "$TMPDIR/cc.log"
    exit 1
fi

if ! AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/probe.ae" >"$TMPDIR/out.log" 2>&1; then
    echo "  [FAIL] std_ipc_socket"
    tail -40 "$TMPDIR/out.log" | sed 's/^/    /'
    exit 1
fi

if ! grep -q "2 passing" "$TMPDIR/out.log"; then
    echo "  [FAIL] std_ipc_socket: not both cases passed"
    tail -40 "$TMPDIR/out.log" | sed 's/^/    /'
    exit 1
fi

echo "  [PASS] std_ipc_socket"
exit 0
