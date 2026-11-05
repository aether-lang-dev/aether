#!/bin/sh
# The reverse proxy must not forward a header whose value carries a bare CR or
# LF. Such a value does not end a line on the wire, so it reaches the code that
# writes the client's head intact; written out verbatim it would end that head
# early and let the bytes behind it be read as headers the upstream never sent
# (CWE-113, response splitting).
#
# A bare CR in the upstream's head is read as SP by the client's reader,
# which the proxy reads upstreams with (RFC 9110 5.5, RFC 9112 2.2; #3572):
# the proxy forwards the 200, the field with its value as one line, and
# nothing behind the CR becomes a field. A bare LF ends a line, so that
# header is dropped and the rest forwarded.
#
# The upstream here is hostile and speaks raw bytes, which is the only way to
# send a header value an HTTP server would never produce.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

PY="$(sh "$ROOT/tests/scripts/find_python.sh" 2>/dev/null)" || PY=""
if [ -z "$PY" ]; then
    echo "  [SKIP] http_proxy_response_injection: no working Python (a Windows Store alias is not one)"
    exit 0
fi

. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"

TMPDIR="$(mktemp -d)"
PX_PID=""
UP_PID=""
cleanup() {
    for pid in "$PX_PID" "$UP_PID"; do
        stop_server "$pid"
    done
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

# The reverse-proxy suite's driver, not a copy of it: it already has the proxy
# role this test needs, and a second copy would be a second thing to keep in
# step. It also keeps this directory free of any .ae file, which the sweep that
# runs every .ae as a standalone program would otherwise pick up and run with
# no arguments.
SERVER_SRC="$SCRIPT_DIR/../http_reverse_proxy/server.ae"
if [ ! -f "$SERVER_SRC" ]; then
    echo "  [FAIL] missing $SERVER_SRC"; exit 1
fi
if ! AETHER_HOME="$ROOT" "$AE" build "$SERVER_SRC" \
        -o "$TMPDIR/server" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] build:"; head -30 "$TMPDIR/build.log"; exit 1
fi

# Both paths, because they defend against a bare LF separately and a request
# is served by one or the other: the pass-through checks the value and drops
# that header, while the copying path stops parsing the block at the malformed
# line. Testing only whichever happens to be active would leave the other
# unguarded. A bare CR is read as SP before either, so both forward its field
# as one line.
# The hostile upstream, once: it binds port 0 and names the port it landed on,
# which is what the proxy has to be told.
$PY "$SCRIPT_DIR/injection_probe.py" upstream "$TMPDIR/up.port" \
    >"$TMPDIR/up.log" 2>&1 &
UP_PID=$!
deadline=$(($(date +%s) + 15))
while [ ! -f "$TMPDIR/up.port" ]; do
    kill -0 "$UP_PID" 2>/dev/null || {
        echo "  [FAIL] hostile upstream died:"; head -20 "$TMPDIR/up.log"; exit 1; }
    [ "$(date +%s)" -lt "$deadline" ] || {
        echo "  [FAIL] hostile upstream never bound"; exit 1; }
    sleep 0.05
done
UP_PORT=$(cat "$TMPDIR/up.port")

run_one() {
    label="$1"; direct="$2"
    AETHER_PROXY_DIRECT="$direct" AETHER_HOME="$ROOT" "$TMPDIR/server" proxy "$UP_PORT" \
        >"$TMPDIR/px.$label.log" 2>&1 &
    PX_PID=$!

    deadline=$(($(date +%s) + 15))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if ! kill -0 "$PX_PID" 2>/dev/null; then
            echo "  [FAIL] proxy died ($label):"; head -30 "$TMPDIR/px.$label.log"; exit 1
        fi
        if grep -q '^READY ' "$TMPDIR/px.$label.log" 2>/dev/null; then break; fi
        sleep 0.05
    done
    PX_PORT=$(read_ready_port "$TMPDIR/px.$label.log") || exit 1
    wait_port "$PX_PORT" || exit 1

    if ! OUT=$($PY "$SCRIPT_DIR/injection_probe.py" client "$PX_PORT" 2>&1); then
        echo "  [FAIL] http_proxy_response_injection ($label): $OUT"; exit 1
    fi

    stop_server "$PX_PID"
    PX_PID=""
}

run_one pass-through 1
run_one copying 0

echo "  [PASS] http_proxy_response_injection: 2/2 paths - a bare CR in an upstream head reads as SP, a bare LF header is dropped; no injected field reaches the client"
