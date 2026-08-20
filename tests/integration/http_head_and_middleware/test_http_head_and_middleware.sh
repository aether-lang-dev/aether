#!/bin/sh
# HEAD framing and middleware keep-alive (#1653).
#
# Three things this pins, each of which was wrong:
#   - a HEAD response carries no body. It used to carry the GET body, which
#     RFC 9110 forbids and which desynchronises a persistent connection: the
#     client reads those bytes as the head of the next response.
#   - HEAD on a path with only a GET route answers 200, not 404. There were
#     two route lookups and only one had the fallback; the keep-alive path,
#     the one that actually serves, did not.
#   - a response produced by a middleware that answers keeps the connection
#     open. That path sent its response and closed, whatever the server's
#     keep-alive setting said, which is why the load balancer (whose proxy
#     answers exactly this way) closed every inbound connection.

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_head_and_middleware — HTTP server code is platform-independent; covered by the POSIX matrix"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v curl >/dev/null 2>&1; then
    echo "  [SKIP] curl not on PATH"
    exit 0
fi

TMPDIR="$(mktemp -d)"
cleanup() {
    if [ -n "${SRV_PID:-}" ]; then
        kill "$SRV_PID" 2>/dev/null || true
        wait "$SRV_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; [ -f "$TMPDIR/srv.log" ] && head -20 "$TMPDIR/srv.log"; exit 1; }

AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

URL="http://127.0.0.1:18107"

# Wait for the port to answer, not for a line in the log: the server prints
# READY before it binds, so a port left busy by an earlier run would otherwise
# show up as a confusing failure in the first assertion instead of here.
deadline=$(($(date +%s) + 20))
until curl --silent --max-time 2 -o /dev/null "$URL/" 2>/dev/null; do
    kill -0 "$SRV_PID" 2>/dev/null || fail "server exited before it served"
    [ "$(date +%s)" -lt "$deadline" ] || fail "server never answered on 18107 (port already in use?)"
    sleep 0.1
done

# 1. HEAD on a GET-only route: 200, the Content-Length GET would have sent,
#    and not one byte of body.
head_bytes="$(curl --silent --show-error --max-time 5 -I -D "$TMPDIR/head.hdr" \
    -o /dev/null -w '%{size_download}' "$URL/")" || fail "HEAD request failed"
grep -q "^HTTP/1.1 200" "$TMPDIR/head.hdr" || fail "HEAD answered $(head -1 "$TMPDIR/head.hdr")"
grep -qi "^Content-Length: 13" "$TMPDIR/head.hdr" \
    || fail "HEAD did not report the GET body length: $(grep -i content-length "$TMPDIR/head.hdr")"
[ "$head_bytes" = "0" ] || fail "HEAD returned a $head_bytes-byte body"

# The GET the HEAD stood in for does send those 13 bytes.
get_bytes="$(curl --silent --show-error --max-time 5 -o /dev/null \
    -w '%{size_download}' "$URL/")" || fail "GET request failed"
[ "$get_bytes" = "13" ] || fail "GET returned $get_bytes bytes, expected 13"

# 2 and 3. HEAD, then GET, then a middleware-answered GET, all on ONE
#    connection, spoken directly rather than through a client's verbose
#    output. A body on the HEAD response desynchronises the stream, so the
#    second response would be wrong or absent; a close after any response
#    ends the exchange early and the helper says so.
cc "$SCRIPT_DIR/pipeline_client.c" -o "$TMPDIR/pipeline" 2>"$TMPDIR/cc.log" \
    || { sed 's/^/        /' "$TMPDIR/cc.log" | head -5; fail "could not compile pipeline_client.c"; }

if ! "$TMPDIR/pipeline" 127.0.0.1 18107 >"$TMPDIR/pipe.out" 2>"$TMPDIR/pipe.err"; then
    sed 's/^/        /' "$TMPDIR/pipe.err" | head -5
    fail "the three requests did not complete on one connection"
fi

grep -q "^1 status=200 clen=13 body=$" "$TMPDIR/pipe.out" \
    || fail "HEAD on the shared connection: $(sed -n 1p "$TMPDIR/pipe.out")"
grep -q "^2 status=200 clen=13 body=body-from-get$" "$TMPDIR/pipe.out" \
    || fail "the GET after a HEAD read: $(sed -n 2p "$TMPDIR/pipe.out")"
grep -q "^3 status=200 clen=22 body=answered-by-middleware$" "$TMPDIR/pipe.out" \
    || fail "the middleware-answered request read: $(sed -n 3p "$TMPDIR/pipe.out")"

# The middleware answer must also SAY it is keeping the connection, since that
# is what a client acts on.
curl --silent --show-error --max-time 5 -D "$TMPDIR/mw.hdr" -o "$TMPDIR/mw.body" "$URL/mw" \
    || fail "middleware request failed"
[ "$(cat "$TMPDIR/mw.body")" = "answered-by-middleware" ] || fail "middleware body wrong"
grep -qi "^Connection: keep-alive" "$TMPDIR/mw.hdr" \
    || fail "a middleware-answered response did not advertise keep-alive"

echo "  [PASS] http_head_and_middleware: HEAD sends no body, falls back to GET, and three requests share one connection"
