#!/bin/sh
# Regression (#3738): an HTTP/2 connection could open streams without bound
# and send each an unbounded body, and lived on its worker for ever.
# SETTINGS_MAX_CONCURRENT_STREAMS went out only when the program set it, and
# nghttp2's own local default is unlimited; a stream's DATA was appended
# with no cap while nghttp2's automatic WINDOW_UPDATE kept it flowing. The
# server now advertises 100 streams unless told otherwise, and resets a
# stream (RST_STREAM, CANCEL) whose body passes the server's body limit
# (server_set_max_body, the one request_body applies, #3740).
#
# The rest of #3738: the connection as a whole holds no more than that limit
# of request bodies (a stream past it is refused); a header list past
# 32 KiB is answered 431; a stream whose request does not arrive whole in
# time is reset; a connection with no stream open closes after the idle
# timeout, PINGs or not; and one past its age (server_set_h2_max_age) gets
# GOAWAY and closes once its streams are done.
#
# h2_probe.c speaks raw HTTP/2 frames over cleartext prior knowledge, so
# it can leave a stream open with its body past the limit. Skips when the
# build has no libnghttp2 (server.ae prints NO_H2).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] http_server_h2_limits: ae not built"; exit 0; }

# The probe is raw sockets through tests/lib/raw_socket.h, which needs
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
fail() { echo "  [FAIL] http_server_h2_limits: $1"; exit 1; }

# The probe's bound is the sweep's own: never timeout(1) here (see
# tests/lib/sweep_timeout.sh).
. "$ROOT/tests/lib/sweep_timeout.sh"
sweep_timeout_bound TIMEOUT "$ROOT" 60

"$AE" build "$SCRIPT_DIR/server.ae" -o "$TMP/srv" >"$TMP/b1.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/b1.log"; fail "server did not build"; }
cc -I"$ROOT/tests/lib" "$SCRIPT_DIR/h2_probe.c" -o "$TMP/probe" $RAW_SOCKET_LIBS 2>"$TMP/cc.log" \
    || { cat "$TMP/cc.log"; fail "could not compile h2_probe.c"; }

"$TMP/srv" > "$TMP/srv.log" 2>&1 &
SRV_PID=$!

i=0
while [ "$i" -lt 100 ]; do
    grep -q -e READY -e NO_H2 "$TMP/srv.log" 2>/dev/null && break
    sleep 0.1
    i=$((i + 1))
done
if grep -q NO_H2 "$TMP/srv.log" 2>/dev/null; then
    echo "  [SKIP] http_server_h2_limits: built without libnghttp2"
    exit 0
fi
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
    *"PASS: h2 limits"*) ;;
    *) fail "the probe did not report PASS" ;;
esac

echo "  [PASS] http_server_h2_limits: streams, header lists and bodies bounded per stream and per connection; late requests reset; idle and old connections closed"
