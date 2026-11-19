#!/bin/sh
# Regression (#3739): what a WebSocket peer can make std.http's server hold.
#
# The receive allocated each frame at the length it declared (up to 1 GiB)
# before a byte of it arrived, reassembled fragments with no total bound in
# an int that overflowed past 1 GiB, and answered pings and closes of any
# size, fragmented or not. Now a message has a cap (64 MiB by default,
# server_set_ws_max_message and ws_set_max_message to change it) checked
# against each frame's declared length, closing with 1009; a frame's bytes
# are read into a buffer that grows as they arrive; and a control frame over
# 125 bytes or fragmented closes with 1002 (RFC 6455 5.5).
#
# #3759: a frame with a reserved opcode, and an unmasked frame from a
# client, close with 1002 (RFC 6455 5.2 and 5.1); so does a masked frame
# from the server, at the client.
#
# #3764: a frame with RSV1, RSV2 or RSV3 set (no extension is negotiated),
# a continuation frame with no message in progress and a data frame inside
# a fragmented message close with 1002 (RFC 6455 5.2, 5.4); a ping between
# two fragments is still answered and the message still reassembles.
#
# A text message, checked across its fragments, and a close reason must be
# UTF-8 (RFC 6455 8.1): no overlong form, surrogate or code point past
# U+10FFFF, and not ending inside a code point. One that is not closes with
# 1007; a binary message is not checked.
#
# ws_probe.c sends the frames over a raw socket, since no library client
# sends them; wsserver.ae is the server. For the client side, `ws_probe
# relay` stands between wsclient.ae and the server: it passes the handshake
# through, then sends the client a masked frame in the server's place.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] ws_receive_limits: ae not built"; exit 0; }

# The probe is raw sockets through tests/lib/raw_socket.h, which needs
# Winsock linked on Windows and nothing extra elsewhere.
RAW_SOCKET_LIBS=""
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) RAW_SOCKET_LIBS="-lws2_32" ;;
esac

TMP="$(mktemp -d)"
SRV_PID=""
RELAY_PID=""
cleanup() {
    stop_server "$SRV_PID"
    stop_server "$RELAY_PID"
    rm -rf "$TMP" || :
    return 0
}
trap cleanup EXIT
fail() { echo "  [FAIL] ws_receive_limits: $1"; exit 1; }

# The probe's bound is the sweep's own: never timeout(1) here (see
# tests/lib/sweep_timeout.sh).
. "$ROOT/tests/lib/sweep_timeout.sh"
sweep_timeout_bound TIMEOUT "$ROOT" 60

"$AE" build "$SCRIPT_DIR/wsserver.ae" -o "$TMP/wssrv" >"$TMP/b1.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/b1.log"; fail "server did not build"; }
"$AE" build "$SCRIPT_DIR/wsclient.ae" -o "$TMP/wscli" >"$TMP/b2.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/b2.log"; fail "client did not build"; }
cc -I"$ROOT/tests/lib" "$SCRIPT_DIR/ws_probe.c" -o "$TMP/probe" $RAW_SOCKET_LIBS 2>"$TMP/cc.log" \
    || { cat "$TMP/cc.log"; fail "could not compile ws_probe.c"; }

"$TMP/wssrv" > "$TMP/srv.log" 2>&1 &
SRV_PID=$!

i=0
while [ "$i" -lt 100 ]; do
    grep -q READY "$TMP/srv.log" 2>/dev/null && break
    sleep 0.1
    i=$((i + 1))
done
grep -q READY "$TMP/srv.log" 2>/dev/null || {
    sed -n '1,10p' "$TMP/srv.log"
    fail "the server never became READY"
}
PORT=$(read_ready_port "$TMP/srv.log") || exit 1

OUT=$($TIMEOUT "$TMP/probe" "$PORT" 2>&1)
RC=$?
echo "$OUT"
if [ "$RC" != "0" ]; then
    sed 's/^/         server: /' "$TMP/srv.log"
    fail "the probe exited $RC"
fi
case "$OUT" in
    *"PASS: ws receive limits"*) ;;
    *) fail "the probe did not report PASS" ;;
esac

# The client side: a masked frame from the server closes with 1002.
"$TMP/probe" relay "$PORT" > "$TMP/relay.log" 2>&1 &
RELAY_PID=$!
i=0
while [ "$i" -lt 100 ]; do
    grep -q READY "$TMP/relay.log" 2>/dev/null && break
    sleep 0.1
    i=$((i + 1))
done
RELAY_PORT=$(read_ready_port "$TMP/relay.log") || exit 1
CLI=$(AE_TEST_PORT="$RELAY_PORT" $TIMEOUT "$TMP/wscli" 2>&1)
CLI_RC=$?
echo "$CLI"
# A client that never connected leaves the relay in accept: stop it rather
# than wait on it.
[ "$CLI_RC" = "0" ] || { sed -n '2,$p' "$TMP/relay.log"; fail "the client exited $CLI_RC"; }
wait "$RELAY_PID"
RELAY_RC=$?
RELAY_PID=""
sed -n '2,$p' "$TMP/relay.log"
[ "$RELAY_RC" = "0" ] || fail "the relay exited $RELAY_RC"
grep -q "PASS: a client closes with 1002 on a masked server frame" "$TMP/relay.log" \
    || fail "the client did not close with 1002 on a masked server frame"

echo "  [PASS] ws_receive_limits: message cap (1009), control frames, reserved opcodes, masking, reserved bits and fragment order (1002), UTF-8 (1007), growth as bytes arrive"
