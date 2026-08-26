#!/bin/sh
# http.request_remote_port / local_addr / local_port / scheme /
# is_tls / http_version — connection-level accessors that complement
# remote_addr. Verifies the values match what the kernel and the
# wire-bytes parser actually see for a cleartext loopback request.
#
# Skipped on Windows for the same reason as http_real_ip /
# http_request_remote_addr: server code is platform-independent
# userland C; curl-over-MSYS2 spawn cost is many times POSIX's.

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_request_conn_accessors — covered by POSIX matrix"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v curl >/dev/null 2>&1; then
    echo "  [SKIP] curl not on PATH"; exit 0
fi

TMPDIR="$(mktemp -d)"
SRV_PID=""
cleanup() {
    if [ -n "$SRV_PID" ]; then
        kill "$SRV_PID" 2>/dev/null || true
        wait "$SRV_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

deadline=$(($(date +%s) + 15))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if grep -q READY "$TMPDIR/srv.log" 2>/dev/null; then break; fi
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        echo "  [FAIL] server died:"; head -30 "$TMPDIR/srv.log"; exit 1
    fi
    sleep 0.1
done
sleep 0.3

URL="http://127.0.0.1:18294/whoami"

RESP=$(curl --silent --show-error --max-time 5 "$URL" 2>"$TMPDIR/c.err") || {
    echo "  [FAIL] curl failed:"; cat "$TMPDIR/c.err"; exit 1
}

# Parse out each field once so failures pinpoint which accessor is wrong.
get_field() { echo "$RESP" | awk -v key="$1" -F'|' '{ for (i=1;i<=NF;i++) { split($i,a,"="); if (a[1]==key) print a[2] } }'; }

PEER=$(get_field peer)
PEER_PORT_NZ=$(get_field peer_port_nz)
LOCAL=$(get_field local)
LOCAL_PORT=$(get_field local_port)
SCHEME=$(get_field scheme)
IS_TLS=$(get_field is_tls)
VER=$(get_field ver)

[ "$PEER" = "127.0.0.1" ] || { echo "  [FAIL] peer expected 127.0.0.1, got '$PEER'"; echo "  raw: $RESP"; exit 1; }
[ "$PEER_PORT_NZ" = "1" ] || { echo "  [FAIL] peer_port should be nonzero (kernel-assigned ephemeral), got '$PEER_PORT_NZ'"; echo "  raw: $RESP"; exit 1; }
[ "$LOCAL" = "127.0.0.1" ] || { echo "  [FAIL] local expected 127.0.0.1, got '$LOCAL'"; echo "  raw: $RESP"; exit 1; }
[ "$LOCAL_PORT" = "18294" ] || { echo "  [FAIL] local_port expected 18294, got '$LOCAL_PORT'"; echo "  raw: $RESP"; exit 1; }
[ "$SCHEME" = "http" ] || { echo "  [FAIL] scheme expected http (cleartext listener), got '$SCHEME'"; echo "  raw: $RESP"; exit 1; }
[ "$IS_TLS" = "0" ] || { echo "  [FAIL] is_tls expected 0 (cleartext listener), got '$IS_TLS'"; echo "  raw: $RESP"; exit 1; }
[ "$VER" = "HTTP/1.1" ] || { echo "  [FAIL] ver expected HTTP/1.1, got '$VER'"; echo "  raw: $RESP"; exit 1; }

# The addresses are resolved once per CONNECTION and cached (#1719), not
# fetched per request. That is only correct if every request on a reused
# connection still reports them — a cache populated for request 1 and not
# read back for request 2 would leave the later ones empty, which no
# single-request check would notice.
#
# curl's multi-URL form reuses one connection, so this drives three
# requests down one socket and requires all three to agree. The peer PORT
# is the sharpest field here: it is kernel-assigned per connection, so if
# the cache were somehow refreshed mid-connection it would still match,
# but if it were dropped the field would go empty.
MULTI=$(curl --silent --show-error --max-time 5 "$URL" "$URL" "$URL" 2>"$TMPDIR/m.err") || {
    echo "  [FAIL] keep-alive curl failed:"; cat "$TMPDIR/m.err"; exit 1
}

# The three bodies concatenate with no separator, so a line-oriented
# count would see "ver=HTTP/1.1peer=127.0.0.1" as one field. Count
# occurrences instead.
# `wc -l` pads its count with leading blanks on BSD/macOS ("       3"),
# so a string compare against "3" passes on GNU and fails on Darwin --
# which is exactly how this test went red on both macOS legs and green
# everywhere else. Strip the padding rather than compare numerically, so
# the failure message still prints a clean count.
MULTI_PEERS=$(echo "$MULTI" | grep -o 'peer=127\.0\.0\.1' | wc -l | tr -d '[:space:]')
[ "$MULTI_PEERS" = "3" ] || {
    echo "  [FAIL] expected peer=127.0.0.1 on all 3 keep-alive requests, got $MULTI_PEERS"
    echo "  raw: $MULTI"; exit 1; }

MULTI_LOCALS=$(echo "$MULTI" | grep -o 'local_port=18294' | wc -l | tr -d '[:space:]')
[ "$MULTI_LOCALS" = "3" ] || {
    echo "  [FAIL] expected local_port=18294 on all 3 keep-alive requests, got $MULTI_LOCALS"
    echo "  raw: $MULTI"; exit 1; }

echo "  [PASS] http_request_conn_accessors (remote_port + local_addr/port + scheme + is_tls + http_version; cached addrs survive keep-alive)"
