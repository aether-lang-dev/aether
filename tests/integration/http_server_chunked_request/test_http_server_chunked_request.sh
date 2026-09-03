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
#
# Not nc: the server answers 413 and closes while the sender is still writing,
# so nc dies of EPIPE and can exit before it has drained the response it just
# provoked. That made this case pass on an idle machine and fail on a busy one,
# blaming the server for a race in the client. This writes until the socket
# refuses more, stops writing, and only then reads, which is the exchange the
# assertion is actually about.
if command -v python3 >/dev/null 2>&1; then
    python3 - "$PORT" > "$TMPDIR/flood.out" 2>/dev/null <<'FLOOD'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=10)
s.sendall(b"POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n")
chunk = b"10000\r\n" + b"a" * 65536 + b"\r\n"
try:
    for _ in range(180):
        s.sendall(chunk)
except OSError:
    pass                      # the server refused the body and closed: expected
try:
    s.shutdown(socket.SHUT_WR)
except OSError:
    pass
buf = b""
try:
    while True:
        b_ = s.recv(65536)
        if not b_:
            break
        buf += b_
except OSError:
    pass
sys.stdout.write(buf.decode("latin1"))
FLOOD
    grep -q "^HTTP/1.1 413" "$TMPDIR/flood.out" \
        || { head -3 "$TMPDIR/flood.out"; fail "an endless chunked body was not refused"; }
else
    echo "  [SKIP] endless-chunked-body case: python3 not on PATH"
fi

# Framing that has no single answer is refused rather than guessed at. Two
# lengths that disagree, or a value that is not a count of bytes, is what lets
# a front end and this server disagree about where a request ends.
expect_400() {                    # expect_400 <label> <payload>
    printf '%b' "$2" > "$TMPDIR/case.req"
    send "$TMPDIR/case.req" "$TMPDIR/case.out"
    grep -q "^HTTP/1.1 400" "$TMPDIR/case.out" \
        || { head -2 "$TMPDIR/case.out"; fail "$1 was not refused"; }
}
expect_body() {                   # expect_body <label> <payload> <expected>
    printf '%b' "$2" > "$TMPDIR/case.req"
    send "$TMPDIR/case.req" "$TMPDIR/case.out"
    grep -q "$3" "$TMPDIR/case.out" \
        || { cat "$TMPDIR/case.out"; fail "$1"; }
}

expect_400 "two Content-Length headers that disagree" \
    'POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 11\r\nConnection: close\r\n\r\nhello world'
expect_400 "a negative Content-Length" \
    'POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\nConnection: close\r\n\r\nhello'
expect_400 "a Content-Length that is not a number" \
    'POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 5abc\r\nConnection: close\r\n\r\nhello'

# A header value mentioning another header is not that header. A search that
# is not anchored to the start of a line reads this as the framing.
expect_body "a Content-Length inside another header's value was read as framing" \
    'POST /upload HTTP/1.1\r\nHost: x\r\nX-Note: Content-Length: 99\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello' \
    'len=5 body=\[hello\]'

# Duplicates that agree say the same thing once, which RFC 9112 allows.
expect_body "identical duplicate Content-Length headers were refused" \
    'POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello' \
    'len=5 body=\[hello\]'

# A header line this server would read differently from whoever sent it is
# refused, because that disagreement is where smuggling lives.
expect_400 "whitespace between a header name and its colon" \
    'POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length : 5\r\nConnection: close\r\n\r\nhello'
expect_400 "an obs-fold continuation line" \
    'POST /upload HTTP/1.1\r\nHost: x\r\nX-Fold: a\r\n b\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello'

# The request line and each header line are copied into fixed buffers. An
# over-long one used to be copied in anyway, off the end of the stack frame,
# which any client could reach with a long URL or a long header value. The
# assertion is as much that the server is still answering afterwards.
LONG_PATH=$(head -c 5000 /dev/zero | tr '\0' 'A')
printf 'GET /%s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' "$LONG_PATH" > "$TMPDIR/longline.req"
send "$TMPDIR/longline.req" "$TMPDIR/longline.out"
grep -q "^HTTP/1.1 414" "$TMPDIR/longline.out" \
    || { head -2 "$TMPDIR/longline.out"; fail "an over-long request line was not answered 414"; }

LONG_HDR=$(head -c 3000 /dev/zero | tr '\0' 'B')
printf 'GET /upload HTTP/1.1\r\nHost: x\r\nX-Long: %s\r\nConnection: close\r\n\r\n' "$LONG_HDR" > "$TMPDIR/longhdr.req"
send "$TMPDIR/longhdr.req" "$TMPDIR/longhdr.out"
grep -q "^HTTP/1.1 431" "$TMPDIR/longhdr.out" \
    || { head -2 "$TMPDIR/longhdr.out"; fail "an over-long header line was not answered 431"; }

# Still serving after all of the above, which is the point of the two checks.
expect_body "the server stopped serving after the oversized requests" \
    'POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello' \
    'len=5 body=\[hello\]'

# More headers than the parser holds used to have the excess dropped in
# silence, so a handler or a middleware inspecting one of them saw it as
# absent. Padding a request past the count was a way to hide a header from
# whatever reads it.
{
    printf 'GET /upload HTTP/1.1\r\nHost: x\r\n'
    i=0
    while [ "$i" -lt 60 ]; do printf 'X-Pad%d: v\r\n' "$i"; i=$((i + 1)); done
    printf 'X-Marker: present\r\nConnection: close\r\n\r\n'
} > "$TMPDIR/manyhdr.req"
send "$TMPDIR/manyhdr.req" "$TMPDIR/manyhdr.out"
grep -q "^HTTP/1.1 431" "$TMPDIR/manyhdr.out" \
    || { head -2 "$TMPDIR/manyhdr.out"; fail "a request with more headers than the parser holds was not refused"; }

# The count that fits is still served, headers and all, so the limit refuses
# rather than simply becoming a smaller limit.
{
    printf 'POST /upload HTTP/1.1\r\nHost: x\r\n'
    i=0
    while [ "$i" -lt 40 ]; do printf 'X-Pad%d: v\r\n' "$i"; i=$((i + 1)); done
    printf 'Content-Length: 5\r\nConnection: close\r\n\r\nhello'
} > "$TMPDIR/fithdr.req"
send "$TMPDIR/fithdr.req" "$TMPDIR/fithdr.out"
grep -q "len=5 body=\[hello\]" "$TMPDIR/fithdr.out" \
    || { cat "$TMPDIR/fithdr.out"; fail "a request within the header count was not served"; }

echo "  [PASS] http_server_chunked_request: framing, header shape, size and count all refused safely"
