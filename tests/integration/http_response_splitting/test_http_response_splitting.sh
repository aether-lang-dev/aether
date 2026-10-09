#!/bin/sh
# Integration test: a handler cannot split the response with a line ending.
#
# A CR LF in a response header is written into the head verbatim and read back
# by the client as a header of its own; a doubled one ends the head and starts
# a second response, which is how a cache is poisoned (CWE-113). Applications
# reflect user input into headers routinely, so the server refuses the bytes.
#
# The assertion reads the raw bytes off the socket, because the whole point is
# what the client would parse, not what the handler intended.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
PORT=18401

if [ ! -x "$AE" ]; then
    echo "  [SKIP] http_response_splitting: ae not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
SRV_PID=""
cleanup() { [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null; rm -rf "$TMPDIR"; }
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

# The probe is tests/lib/raw_exchange.c rather than nc, which neither Windows
# nor every Linux runner has. It needs Winsock linked on Windows.
RAW_SOCKET_LIBS=""
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) RAW_SOCKET_LIBS="-lws2_32" ;;
esac
cc -I"$ROOT/tests/lib" "$ROOT/tests/lib/raw_exchange.c" -o "$TMPDIR/raw_exchange" $RAW_SOCKET_LIBS 2>"$TMPDIR/cc.log" \
    || { cat "$TMPDIR/cc.log"; fail "could not compile raw_exchange.c"; }

AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/server.ae" > "$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

printf 'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' > "$TMPDIR/req.txt"
i=0
while [ "$i" -lt 100 ]; do
    "$TMPDIR/raw_exchange" "$PORT" "$TMPDIR/req.txt" "$TMPDIR/resp.txt" 2>/dev/null
    grep -q "^HTTP/1.1" "$TMPDIR/resp.txt" 2>/dev/null && break
    sleep 0.1
    i=$((i + 1))
done
grep -q "^HTTP/1.1" "$TMPDIR/resp.txt" 2>/dev/null || { cat "$TMPDIR/srv.log"; fail "server did not answer"; }

if grep -q "X-Injected" "$TMPDIR/resp.txt"; then
    cat "$TMPDIR/resp.txt"
    fail "an injected header reached the client"
fi
grep -q "^Content-Type: text/plain" "$TMPDIR/resp.txt" \
    || { cat "$TMPDIR/resp.txt"; fail "a legitimate header was dropped too"; }

echo "  [PASS] http_response_splitting: a header carrying a line ending is not emitted"
