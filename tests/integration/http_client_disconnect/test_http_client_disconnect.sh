#!/bin/sh
# A client that goes away mid-answer must not take the server with it.
#
# A write to a peer that has closed raises SIGPIPE, whose default disposition
# is to kill the process. Nothing in the server ignored it and only one send in
# the whole codebase passed MSG_NOSIGNAL, so a client pressing stop could end
# the server: found by putting a TLS listener under load, where it died at
# eight concurrent connections with exit status 141.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
SRC="$ROOT/tests/integration/http_reverse_proxy/server.ae"

command -v python3 >/dev/null 2>&1 || { echo "  [SKIP] python3 not on PATH"; exit 0; }

TMPDIR="$(mktemp -d)"
UP_PID=""; PX_PID=""
cleanup() {
    [ -n "$UP_PID" ] && { kill "$UP_PID" 2>/dev/null || true; wait "$UP_PID" 2>/dev/null || true; }
    [ -n "$PX_PID" ] && { kill "$PX_PID" 2>/dev/null || true; wait "$PX_PID" 2>/dev/null || true; }
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SRC" -o "$TMPDIR/server" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] build:"; head -20 "$TMPDIR/build.log"; exit 1
fi

AETHER_HOME="$ROOT" "$TMPDIR/server" upstream >"$TMPDIR/up.log" 2>&1 &
UP_PID=$!
AETHER_HOME="$ROOT" "$TMPDIR/server" proxy >"$TMPDIR/px.log" 2>&1 &
PX_PID=$!

deadline=$(($(date +%s) + 15))
while [ "$(date +%s)" -lt "$deadline" ]; do
    curl -s -o /dev/null --max-time 1 "http://127.0.0.1:19000/echo" 2>/dev/null && break
    sleep 0.2
done
curl -s -o /dev/null --max-time 3 "http://127.0.0.1:19000/echo" || {
    echo "  [FAIL] proxy never answered"; head -20 "$TMPDIR/px.log"; exit 1; }

if OUT=$(python3 "$SCRIPT_DIR/disconnect_probe.py" 19000 2>&1); then
    :
else
    echo "  [FAIL] $OUT"
    if kill -0 "$PX_PID" 2>/dev/null; then
        echo "         the proxy is still running, so the answer itself was wrong"
    else
        wait "$PX_PID" 2>/dev/null
        echo "         the proxy exited with status $? (141 is SIGPIPE)"
        PX_PID=""
    fi
    exit 1
fi

kill -0 "$PX_PID" 2>/dev/null || { echo "  [FAIL] the proxy died during the run"; PX_PID=""; exit 1; }
echo "  [PASS] http_client_disconnect: 40 clients reset mid-answer, the proxy served the next request"
