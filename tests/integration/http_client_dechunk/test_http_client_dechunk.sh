#!/bin/sh
# Integration test: std.http.client transparently de-chunks a
# Transfer-Encoding: chunked response (vcr_record_chunked_dechunk_wish.md,
# stdlib-client half). A raw-socket C upstream replies chunked (the Aether
# server can't — it always sets Content-Length); the Aether driver asserts
# the client returns the decoded payload, not chunk framing.
#
# Split out of the former vcr_record_chunked test when the VCR engine moved
# to the servirtium-vcr monorepo (the record/replay half lives there now).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

# The upstream is raw sockets through tests/lib/raw_socket.h, which needs
# Winsock linked on Windows and nothing extra elsewhere.
RAW_SOCKET_LIBS=""
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) RAW_SOCKET_LIBS="-lws2_32" ;;
esac

TMPDIR="$(mktemp -d)"
UP_PID=""
cleanup() { [ -n "$UP_PID" ] && kill "$UP_PID" 2>/dev/null; rm -rf "$TMPDIR"; }
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

# Build + start the chunked upstream; it prints its OS-assigned port.
cc -I"$ROOT/tests/lib" "$SCRIPT_DIR/chunked_upstream.c" -o "$TMPDIR/upstream" $RAW_SOCKET_LIBS 2>"$TMPDIR/cc.log" \
    || { cat "$TMPDIR/cc.log"; fail "could not compile chunked_upstream.c"; }
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
[ -n "$PORT" ] || fail "chunked upstream did not report a port"

OUT="$(UPSTREAM_PORT="$PORT" AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/dechunk.ae" 2>&1)"
if ! echo "$OUT" | grep -q "^PASS"; then
    echo "$OUT"
    fail "std.http.client de-chunk assertion did not pass"
fi

echo "  [PASS] http_client_dechunk: std.http.client de-chunks Transfer-Encoding: chunked"
