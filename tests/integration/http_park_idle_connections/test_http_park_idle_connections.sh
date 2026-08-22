#!/bin/sh
# The server holds far more keep-alive connections than it has workers (#1663).
#
# A worker owned its connection for that connection's whole life, so the
# number a server could hold open was the worker count (cores * 2, capped at
# 64). Past that line, connections that never reached a worker stalled their
# clients: 200 concurrent keep-alive clients measured 99 rps against 71,000
# after parking, and this test's own first run served 300 connections once and
# then only 16 of them a second time, which is the shape of the bug.
#
# So the property is: open many more connections than any plausible worker
# count, serve every one, and then serve every one AGAIN on the same socket.
# The second round is the test. The first only proves the server is up.

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_park_idle_connections — POSIX sockets; covered by the POSIX matrix"
        exit 0
        ;;
esac

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
PORT=18137
CONNECTIONS="${PARK_TEST_CONNECTIONS:-200}"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] http_park_idle_connections: ae not built"
    exit 0
fi

# The client needs a descriptor per connection, plus the shell's own. A box
# with a low limit would fail on the client side and say nothing about the
# server, so ask for headroom and skip if it cannot be had.
limit="$(ulimit -n 2>/dev/null || echo 0)"
if [ "$limit" -lt $((CONNECTIONS + 64)) ] 2>/dev/null; then
    ulimit -n $((CONNECTIONS + 64)) 2>/dev/null || true
    limit="$(ulimit -n 2>/dev/null || echo 0)"
fi
if [ "$limit" -lt $((CONNECTIONS + 64)) ] 2>/dev/null; then
    echo "  [SKIP] http_park_idle_connections: descriptor limit $limit is below $((CONNECTIONS + 64))"
    exit 0
fi

TMP="$(mktemp -d)"
SRV_PID=""
cleanup() {
    if [ -n "$SRV_PID" ]; then
        kill "$SRV_PID" 2>/dev/null || true
        # Reaped here so the shell does not print its own "Terminated" line
        # into a passing test's output.
        wait "$SRV_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; [ -f "$TMP/srv.log" ] && sed 's/^/        /' "$TMP/srv.log" | head -10; exit 1; }

cc "$SCRIPT_DIR/park_client.c" -o "$TMP/client" 2>"$TMP/cc.log" \
    || { sed 's/^/        /' "$TMP/cc.log" | head -5; fail "could not compile park_client.c"; }

AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/server.ae" >"$TMP/srv.log" 2>&1 &
SRV_PID=$!

deadline=$(($(date +%s) + 25))
until curl --silent --max-time 2 -o /dev/null "http://127.0.0.1:$PORT/" 2>/dev/null; do
    kill -0 "$SRV_PID" 2>/dev/null || fail "server exited before it served"
    [ "$(date +%s)" -lt "$deadline" ] || fail "server never answered on $PORT (port already in use?)"
    sleep 0.2
done

out="$("$TMP/client" "$PORT" "$CONNECTIONS" 2>"$TMP/client.err")"
rc=$?
if [ "$rc" -ne 0 ]; then
    printf '%s\n' "$out" | sed 's/^/        /'
    sed 's/^/        /' "$TMP/client.err" | head -8
    fail "$CONNECTIONS keep-alive connections were not all served twice"
fi

printf '%s\n' "$out" | grep -q "^round1 served $CONNECTIONS of $CONNECTIONS$" \
    || fail "first round: $(printf '%s' "$out" | grep round1)"
printf '%s\n' "$out" | grep -q "^round2 served $CONNECTIONS of $CONNECTIONS$" \
    || fail "second round: $(printf '%s' "$out" | grep round2)"

echo "  [PASS] http_park_idle_connections: $CONNECTIONS keep-alive connections held open and served twice"
