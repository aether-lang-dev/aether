#!/bin/sh
# Integration test: what std.http.client puts on the wire.
#
# The request head is the client's contract with every server it talks to, and
# nothing asserted it. The keep-alive test passes even when the client is made
# to send `Connection: close`, because that upstream holds the socket open
# whatever it is told, so the header itself was never checked. This upstream
# answers with the head it received.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] http_client_request_head on Windows (raw POSIX sockets)"
        exit 0
        ;;
esac

if [ ! -x "$AE" ]; then
    echo "  [SKIP] http_client_request_head: ae not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
UP_PID=""
cleanup() { [ -n "$UP_PID" ] && kill "$UP_PID" 2>/dev/null; rm -rf "$TMPDIR"; }
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

cc "$SCRIPT_DIR/head_echo_upstream.c" -o "$TMPDIR/upstream" 2>"$TMPDIR/cc.log" \
    || { cat "$TMPDIR/cc.log"; fail "could not compile head_echo_upstream.c"; }
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

OUT="$(UPSTREAM_PORT="$PORT" AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/request_head.ae" 2>&1)"
if ! echo "$OUT" | grep -q "^PASS"; then
    echo "$OUT"
    fail "the request head was not what the wire contract says"
fi

echo "  [PASS] http_client_request_head: request line, Host, Connection and caller headers"
