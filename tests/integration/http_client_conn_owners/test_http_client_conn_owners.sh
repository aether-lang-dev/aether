#!/bin/sh
# Integration test: std.http.client connections belong to an owner.
#
# The upstream (conn_auth_upstream.c) authenticates CONNECTIONS, as NTLM and
# Negotiate do, and names the connection that carried each response. Through
# 0.938.0 the client kept one idle pool per process keyed by origin alone, so
# a connection one caller authenticated served the next request from anyone
# in the process (servirtium-vcr's recording proxy, 2026-11-23).
#
#   anonymous.ae  a request with no owner never gets another's connection
#   owners.ae     two pools never share; a pinned owner keeps one connection;
#                 two owners in one process do not leak to each other;
#                 freeing an owner closes its connections (counted on both
#                 ends); reuse within one owner still happens
#   proxy.ae      a std.http proxy pairs each client connection with a pinned
#                 upstream connection (http.request_conn_id), freed when the
#                 client connection closes (http.server_on_conn_close)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"

RAW_SOCKET_LIBS=""
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) RAW_SOCKET_LIBS="-lws2_32" ;;
esac

if [ ! -x "$AE" ]; then
    echo "  [SKIP] http_client_conn_owners: ae not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
UP_PID=""
cleanup() { stop_server "$UP_PID"; rm -rf "$TMPDIR"; }
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

cc -I"$ROOT/tests/lib" "$SCRIPT_DIR/conn_auth_upstream.c" -o "$TMPDIR/upstream" $RAW_SOCKET_LIBS 2>"$TMPDIR/cc.log" \
    || { cat "$TMPDIR/cc.log"; fail "could not compile conn_auth_upstream.c"; }

# One upstream for the three programs, started as the other client tests
# start theirs. Each program's connections close when it exits, and the
# upstream's /stats counts only open ones, so they do not see each other's.
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

for prog in anonymous owners proxy; do
    OUT="$(UPSTREAM_PORT="$PORT" AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/$prog.ae" 2>&1)"
    if ! echo "$OUT" | grep -q "^PASS"; then
        echo "$OUT"
        fail "$prog.ae"
    fi
    echo "$OUT" | sed 's/^/    /'
done

echo "  [PASS] http_client_conn_owners: connections stay with their owner"
