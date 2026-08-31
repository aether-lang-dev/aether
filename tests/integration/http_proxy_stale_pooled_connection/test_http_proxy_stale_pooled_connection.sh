#!/bin/sh
# An upstream may close a pooled connection whenever it likes, and the proxy
# only finds out by using it. Every request must still be answered: the request
# has not been delivered, so it goes again down a fresh connection.
#
# Before this was handled, the empty read was parsed as status 0 and the client
# received "HTTP/1.1 0 Unknown", or the connection was dropped with no answer
# at all.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v python3 >/dev/null 2>&1; then
    echo "  [SKIP] http_proxy_stale_pooled_connection: python3 not on PATH"
    exit 0
fi

TMPDIR="$(mktemp -d)"
PX_PID=""
cleanup() {
    if [ -n "$PX_PID" ]; then
        kill "$PX_PID" 2>/dev/null || true
        wait "$PX_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

# The reverse-proxy suite's driver, not a copy: a second copy would be a second
# thing to keep in step, and any .ae file here would be picked up by the sweep
# that runs every .ae as a standalone program.
SERVER_SRC="$SCRIPT_DIR/../http_reverse_proxy/server.ae"
if [ ! -f "$SERVER_SRC" ]; then
    echo "  [FAIL] missing $SERVER_SRC"; exit 1
fi
if ! AETHER_HOME="$ROOT" "$AE" build "$SERVER_SRC" \
        -o "$TMPDIR/server" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] build:"; head -30 "$TMPDIR/build.log"; exit 1
fi

# The probe binds 19001 itself and answers as the upstream, so only the proxy
# is started here.
AETHER_HOME="$ROOT" "$TMPDIR/server" proxy >"$TMPDIR/px.log" 2>&1 &
PX_PID=$!

deadline=$(($(date +%s) + 15))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if ! kill -0 "$PX_PID" 2>/dev/null; then
        echo "  [FAIL] proxy died:"; head -30 "$TMPDIR/px.log"; exit 1
    fi
    if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:19000/echo" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

if ! OUT=$(python3 "$SCRIPT_DIR/stale_probe.py" 2>&1); then
    echo "  [FAIL] http_proxy_stale_pooled_connection: $OUT"; exit 1
fi

echo "  [PASS] http_proxy_stale_pooled_connection: 60/60 answered when the upstream closes every pooled connection"
