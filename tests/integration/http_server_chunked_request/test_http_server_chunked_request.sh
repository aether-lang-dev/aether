#!/bin/sh
# Integration test: chunked request bodies, and the framing pair that is not
# allowed to be resolved.
#
# Transfer-Encoding decides where a body ends, and when it is present
# Content-Length does not (RFC 9112 6.3). A server that ignores the header
# reads a chunked upload as having no body: the payload is dropped, and the
# chunk bytes are left in the stream to be parsed as the start of the next
# request. Against a front end that does honour it, that disagreement about
# where a request ends is request smuggling.
#
# A message carrying both lengths is refused rather than resolved, because
# the two lengths are what a smuggling pair is built from.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
PORT=18407

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] http_server_chunked_request on Windows (raw socket probe)"
        exit 0
        ;;
esac

[ -x "$AE" ] || { echo "  [SKIP] http_server_chunked_request: ae not built"; exit 0; }
command -v nc >/dev/null 2>&1 || { echo "  [SKIP] http_server_chunked_request: nc not available"; exit 0; }

TMPDIR="$(mktemp -d)"
SRV_PID=""
cleanup() { [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null; rm -rf "$TMPDIR"; }
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/server.ae" > "$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

send() {                          # send <payload-file> <out-file>
    nc 127.0.0.1 "$PORT" < "$1" > "$2" 2>/dev/null
}

printf 'POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n' > "$TMPDIR/chunked.req"
printf 'POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5\r\nhello\r\n0\r\n\r\n' > "$TMPDIR/both.req"
printf 'POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\nConnection: close\r\n\r\nhello world' > "$TMPDIR/plain.req"

i=0
while [ "$i" -lt 100 ]; do
    send "$TMPDIR/plain.req" "$TMPDIR/plain.out"
    grep -q "^HTTP/1.1" "$TMPDIR/plain.out" 2>/dev/null && break
    sleep 0.1
    i=$((i + 1))
done
grep -q "^HTTP/1.1" "$TMPDIR/plain.out" 2>/dev/null || { cat "$TMPDIR/srv.log"; fail "server did not answer"; }

grep -q "len=11 body=\[hello world\]" "$TMPDIR/plain.out" \
    || { cat "$TMPDIR/plain.out"; fail "a Content-Length body did not arrive intact"; }

send "$TMPDIR/chunked.req" "$TMPDIR/chunked.out"
grep -q "len=11 body=\[hello world\]" "$TMPDIR/chunked.out" \
    || { cat "$TMPDIR/chunked.out"; fail "a chunked body did not reach the handler decoded"; }

send "$TMPDIR/both.req" "$TMPDIR/both.out"
grep -q "^HTTP/1.1 400" "$TMPDIR/both.out" \
    || { cat "$TMPDIR/both.out"; fail "a request carrying both lengths was not refused"; }

# Anything after the terminal chunk belongs to the next request, not to this
# body. A decoder that assumed the body ran to the end of what it had read
# would swallow it.
printf 'POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\nPOST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\nConnection: close\r\n\r\nSECOND!' > "$TMPDIR/pipelined.req"
send "$TMPDIR/pipelined.req" "$TMPDIR/pipelined.out"
grep -q "len=5 body=\[hello\]" "$TMPDIR/pipelined.out" \
    || { cat "$TMPDIR/pipelined.out"; fail "the chunked body of a pipelined pair was wrong"; }
grep -q "len=7 body=\[SECOND!\]" "$TMPDIR/pipelined.out" \
    || { cat "$TMPDIR/pipelined.out"; fail "the request pipelined after a chunked body was lost"; }

# A chunked body declares no length up front, so a sender that never sends the
# terminal chunk is bounded only by what the server refuses to hold.
{
    printf 'POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n'
    i=0
    while [ "$i" -lt 180 ]; do
        printf '10000\r\n'
        head -c 65536 /dev/zero | tr '\0' 'a'
        printf '\r\n'
        i=$((i + 1))
    done
} | nc 127.0.0.1 "$PORT" > "$TMPDIR/flood.out" 2>/dev/null
grep -q "^HTTP/1.1 413" "$TMPDIR/flood.out" \
    || { head -3 "$TMPDIR/flood.out"; fail "an endless chunked body was not refused"; }

echo "  [PASS] http_server_chunked_request: decode, pipelining, size limit, both-lengths refused"
