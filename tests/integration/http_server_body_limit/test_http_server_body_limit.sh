#!/bin/sh
# Regression (#3740): http.request_body buffered whatever Content-Length the
# client declared, up to LONG_MAX, so a few connections each declaring a
# multi-GB body grew the server until it ran out of memory. The server now
# has a limit on a buffered body (64 MiB by default, server_set_max_body to
# change it): request_body refuses a body past it without reading it, and
# the server answers 413. A handler that streams its body with
# request_body_read is not bounded by it, so large uploads keep working.
#
# body_probe.c sends the requests over a raw socket, so it can declare a
# body it never sends; server.ae is the server, with a limit of 100000.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] http_server_body_limit: ae not built"; exit 0; }

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
fail() { echo "  [FAIL] http_server_body_limit: $1"; exit 1; }

# The probe's bound is the sweep's own: never timeout(1) here (see
# tests/lib/sweep_timeout.sh).
. "$ROOT/tests/lib/sweep_timeout.sh"
sweep_timeout_bound TIMEOUT "$ROOT" 60

"$AE" build "$SCRIPT_DIR/server.ae" -o "$TMP/srv" >"$TMP/b1.log" 2>&1 \
    || { sed -n '1,10p' "$TMP/b1.log"; fail "server did not build"; }
cc -I"$ROOT/tests/lib" "$SCRIPT_DIR/body_probe.c" -o "$TMP/probe" $RAW_SOCKET_LIBS 2>"$TMP/cc.log" \
    || { cat "$TMP/cc.log"; fail "could not compile body_probe.c"; }

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
    *"PASS: body limit"*) ;;
    *) fail "the probe did not report PASS" ;;
esac

echo "  [PASS] http_server_body_limit: 413 past the limit, streaming unbounded, 64-bit declared lengths (#3761)"
