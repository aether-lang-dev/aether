#!/bin/sh
# Regression for #1086: CONNECT handlers can accept a tunnel, obtain
# a std.tcp socket, and relay binary bytes including embedded NULs.

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_server_connect_tunnel - socket-heavy HTTP server behaviour is covered by POSIX matrix"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
AETHER_CACHE_DIR="$TMPDIR/cache"
export AETHER_CACHE_DIR
SRV_PID=""
cleanup() {
    if [ -n "$SRV_PID" ]; then
        kill "$SRV_PID" 2>/dev/null || true
        wait "$SRV_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

deadline=$(($(date +%s) + 15))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if grep -q READY "$TMPDIR/srv.log" 2>/dev/null; then break; fi
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        echo "  [FAIL] server died:"
        head -30 "$TMPDIR/srv.log"
        exit 1
    fi
    sleep 0.1
done
grep -q READY "$TMPDIR/srv.log" 2>/dev/null || {
    echo "  [FAIL] server never became ready"
    head -30 "$TMPDIR/srv.log"
    exit 1
}
sleep 0.3

AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/client.ae" >"$TMPDIR/client.out" 2>&1 || {
    echo "  [FAIL] client exited non-zero:"
    cat "$TMPDIR/client.out"
    echo "--- server log ---"
    head -30 "$TMPDIR/srv.log"
    exit 1
}

cat "$TMPDIR/client.out"
