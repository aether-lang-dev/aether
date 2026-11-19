#!/bin/sh
# Regression (#3737, the request head): the only bound on reading a request
# head was the per-read timeout, so a client that sent one byte just inside
# it, over and over, held a worker for as long as it liked; a few such
# clients per worker held them all (slowloris). A head now has to arrive
# whole within the server's head timeout from its first byte (30 s by
# default, server_set_head_timeout to change it), and is answered 408 past
# it.
#
# slow_probe.c is the slow client, over a raw socket; server.ae is the
# server, with a 1 s read timeout and a 2 s head timeout.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] http_server_head_deadline: ae not built"; exit 0; }

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
fail() { echo "  [FAIL] http_server_head_deadline: $1"; exit 1; }

# The probe's bound is the sweep's own: never timeout(1) here (see
# tests/lib/sweep_timeout.sh).
. "$ROOT/tests/lib/sweep_timeout.sh"
sweep_timeout_bound TIMEOUT "$ROOT" 60

"$AE" build "$SCRIPT_DIR/server.ae" -o "$TMP/srv" >"$TMP/b1.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/b1.log"; fail "server did not build"; }
cc -I"$ROOT/tests/lib" "$SCRIPT_DIR/slow_probe.c" -o "$TMP/probe" $RAW_SOCKET_LIBS 2>"$TMP/cc.log" \
    || { cat "$TMP/cc.log"; fail "could not compile slow_probe.c"; }

"$TMP/srv" > "$TMP/srv.log" 2>&1 &
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
    *"PASS: head deadline"*) ;;
    *) fail "the probe did not report PASS" ;;
esac

echo "  [PASS] http_server_head_deadline: a head past its deadline is answered 408"
