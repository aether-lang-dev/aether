#!/bin/sh
# Integration test (#1653): std.http.client reuses an upstream connection.
#
# The client closed the socket after every response, so a proxy paid a TCP
# handshake per request; measured against nginx that churn was the whole of
# std.http.server.lb's 3.7x deficit. The upstream here accepts exactly one
# connection and then closes its listener, so requests two and three can only
# be answered over the connection request one opened, and the request after
# `client_pool_clear()` must fail. Nothing about this passes by accident.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] http_client_keepalive on Windows (raw POSIX sockets)"
        exit 0
        ;;
esac

if [ ! -x "$AE" ]; then
    echo "  [SKIP] http_client_keepalive: ae not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
UP_PID=""
cleanup() { [ -n "$UP_PID" ] && kill "$UP_PID" 2>/dev/null; rm -rf "$TMPDIR"; }
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

cc "$SCRIPT_DIR/keepalive_upstream.c" -o "$TMPDIR/upstream" 2>"$TMPDIR/cc.log" \
    || { cat "$TMPDIR/cc.log"; fail "could not compile keepalive_upstream.c"; }
"$TMPDIR/upstream" > "$TMPDIR/port.txt" 2>/dev/null &
UP_PID=$!

PORT=""
i=0
while [ "$i" -lt 100 ]; do
    PORT="$(head -n1 "$TMPDIR/port.txt" 2>/dev/null)"
    [ -n "$PORT" ] && break
    sleep 0.05
    i=$((i + 1))
done
[ -n "$PORT" ] || fail "upstream did not report a port"

OUT="$(UPSTREAM_PORT="$PORT" AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/keepalive.ae" 2>&1)"
if ! echo "$OUT" | grep -q "^PASS"; then
    echo "$OUT"
    fail "std.http.client did not reuse the connection"
fi

echo "  [PASS] http_client_keepalive: three requests over one upstream connection"
