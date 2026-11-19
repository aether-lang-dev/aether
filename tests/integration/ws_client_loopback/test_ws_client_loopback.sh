#!/bin/sh
# WebSocket CLIENT loopback: dial the shipped WS server and round-trip.
#
# std.http had a complete RFC 6455 server but nothing that ORIGINATES a
# connection, so a ws:// endpoint was unreachable
# (asks/websocket-client-for-bidi.md — WebDriver-BiDi is transported over a
# socket the client opens).
#
# The peer here is deliberately the server side of the SAME stack: that is
# what proves interoperability rather than self-consistency. In particular a
# client MUST mask every frame it sends and a server MUST NOT (RFC 6455 §5.3),
# so an echo that comes back intact is evidence the masking is right — a
# wrongly-masked frame would reach the server as garbage.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] ws_client_loopback: ae not built"; exit 0; }

TMP="$(mktemp -d)"
SRV_PID=""
cleanup() {
    # `|| :` on every line: under `set -e` a failing command inside a trap
    # aborts the function, so a kill of an already-dead server would both skip
    # the rm below (leaking the temp dir) and leave a non-zero status for the
    # script to exit with -- reporting a failure after having printed [PASS].
    stop_server "$SRV_PID"
    rm -rf "$TMP" || :
    return 0
}
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

# A backstop rather than the actual safety net (the client sets its own recv
# timeout), and the sweep's own bound: never timeout(1) here, which ends the
# client by signalling it, and on Windows a signal to a native program's MSYS2
# stand-in reaches unrelated MSYS2 shells (#3154, tests/lib/sweep_timeout.sh).
. "$ROOT/tests/lib/sweep_timeout.sh"
sweep_timeout_bound TIMEOUT "$ROOT" 30

"$AE" build "$SCRIPT_DIR/wsserver.ae" -o "$TMP/wssrv" >"$TMP/b1.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/b1.log"; fail "server did not build"; }
"$AE" build "$SCRIPT_DIR/wsclient.ae" -o "$TMP/wscli" >"$TMP/b2.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/b2.log"; fail "client did not build"; }

"$TMP/wssrv" > "$TMP/srv.log" 2>&1 &
SRV_PID=$!

# Wait for the listener rather than sleeping a fixed amount: a fixed sleep is
# the classic source of loaded-CI flakes in this suite.
i=0
while [ "$i" -lt 60 ]; do
    grep -q READY "$TMP/srv.log" 2>/dev/null && break
    sleep 0.1
    i=$((i + 1))
done
grep -q READY "$TMP/srv.log" 2>/dev/null || {
    sed -n '1,10p' "$TMP/srv.log"
    fail "ws echo server never became READY"
}
PORT=$(read_ready_port "$TMP/srv.log") || exit 1

OUT=$(AE_TEST_PORT="$PORT" $TIMEOUT "$TMP/wscli" 2>&1) || {
    echo "$OUT" | sed 's/^/         /'
    fail "client exited non-zero"
}
case "$OUT" in
    *"PASS: ws round-trip"*) ;;
    *) echo "$OUT" | sed 's/^/         /'; fail "round-trip did not report PASS" ;;
esac
case "$OUT" in
    *"PASS: ws_client_free returns the message buffer to the cap"*) ;;
    *) echo "$OUT" | sed 's/^/         /'; fail "freed client handles stayed counted (#3758)" ;;
esac

echo "  [PASS] ws_client_loopback: ws:// handshake, masked send, echo round-trip, free returns the buffer (#3758)"
