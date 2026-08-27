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

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] http_response_splitting on Windows (raw socket probe)"
        exit 0
        ;;
esac

if [ ! -x "$AE" ]; then
    echo "  [SKIP] http_response_splitting: ae not built"
    exit 0
fi
if ! command -v nc >/dev/null 2>&1; then
    echo "  [SKIP] http_response_splitting: nc not available"
    exit 0
fi

TMPDIR="$(mktemp -d)"
SRV_PID=""
cleanup() { [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null; rm -rf "$TMPDIR"; }
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/server.ae" > "$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

i=0
while [ "$i" -lt 100 ]; do
    printf 'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' \
        | nc 127.0.0.1 "$PORT" > "$TMPDIR/resp.txt" 2>/dev/null
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
