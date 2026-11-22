#!/bin/sh
# Regression (#3750): the HTTP client grew a response's buffer for as long
# as the server kept sending, and read a streamed response's head the same
# way, with no deadline for the whole of it. A response now holds at most
# its request's limit (set_max_response) or the client's default (64 MiB,
# set_default_max_response), and a streamed head is bounded by that limit
# and by the request's timeout. The reverse proxy gives the client its own
# body cap, so a cap past the default keeps working.
#
# resp_server.c is the server (raw socket: big bodies, an endless head, a
# head a byte at a time); client.ae is the client and two proxy gateways.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] http_client_max_response: ae not built"; exit 0; }

# The server is raw sockets through tests/lib/raw_socket.h, which needs
# Winsock linked on Windows and nothing extra elsewhere.
RAW_SOCKET_LIBS=""
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) RAW_SOCKET_LIBS="-lws2_32" ;;
esac

TMP="$(mktemp -d)"
SRV_PID=""
cleanup() {
    stop_server "$SRV_PID"
    rm -rf "$TMP" || :
    return 0
}
trap cleanup EXIT
# The sweep ends a test past its limit with a signal, which skips the EXIT
# trap: the server was left running, and the sweep waited on it. Exiting from
# the signal's trap runs cleanup.
trap 'exit 143' INT TERM HUP
fail() { echo "  [FAIL] http_client_max_response: $1"; exit 1; }

# The client's bound is the sweep's own: never timeout(1) here (see
# tests/lib/sweep_timeout.sh).
. "$ROOT/tests/lib/sweep_timeout.sh"
sweep_timeout_bound TIMEOUT "$ROOT" 120

"$AE" build "$SCRIPT_DIR/client.ae" -o "$TMP/client" >"$TMP/b1.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/b1.log"; fail "client did not build"; }
cc -I"$ROOT/tests/lib" "$SCRIPT_DIR/resp_server.c" -o "$TMP/srv" $RAW_SOCKET_LIBS 2>"$TMP/cc.log" \
    || { cat "$TMP/cc.log"; fail "could not compile resp_server.c"; }

"$TMP/srv" > "$TMP/srv.log" 2>&1 &
SRV_PID=$!

i=0
while [ "$i" -lt 100 ]; do
    grep -q READY "$TMP/srv.log" 2>/dev/null && break
    sleep 0.1
    i=$((i + 1))
done
PORT=$(read_ready_port "$TMP/srv.log") || fail "the server never became READY"

OUT=$(AE_TEST_PORT="$PORT" $TIMEOUT "$TMP/client" 2>&1)
RC=$?
echo "$OUT"
[ "$RC" = "0" ] || fail "the client exited $RC"
case "$OUT" in
    *"PASS: client response limits"*) ;;
    *) fail "the client did not report PASS" ;;
esac

echo "  [PASS] http_client_max_response: buffered responses and streamed heads bounded, the proxy's cap past the default kept"
