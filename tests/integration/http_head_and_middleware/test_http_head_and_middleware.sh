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

deadline=$(($(date +%s) + 15))
while [ "$(date +%s)" -lt "$deadline" ]; do
    grep -q READY "$TMPDIR/srv.log" 2>/dev/null && break
    kill -0 "$SRV_PID" 2>/dev/null || fail "server died"
    sleep 0.1
done
sleep 0.3

URL="http://127.0.0.1:18107"

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

# 2. HEAD then GET on ONE connection. A body on the HEAD response would be
#    read as the start of the GET response, so this fails if 1. regresses.
curl --silent --show-error --max-time 5 -v \
        -I "$URL/" -o "$TMPDIR/p1" \
        --next "$URL/" -o "$TMPDIR/p2" 2>"$TMPDIR/pipe.err" \
    || fail "HEAD-then-GET on one connection failed"
grep -q "Re-using existing connection" "$TMPDIR/pipe.err" \
    || fail "curl did not reuse the connection for the second request"
[ "$(cat "$TMPDIR/p2")" = "body-from-get" ] \
    || fail "the GET after a HEAD read '$(cat "$TMPDIR/p2")'"

# 3. A middleware-answered response keeps the connection open.
curl --silent --show-error --max-time 5 -v \
        "$URL/mw" -o "$TMPDIR/m1" \
        "$URL/mw" -o "$TMPDIR/m2" 2>"$TMPDIR/mw.err" \
    || fail "middleware requests failed"
[ "$(cat "$TMPDIR/m1")" = "answered-by-middleware" ] || fail "middleware body wrong"
grep -qi "^< Connection: keep-alive" "$TMPDIR/mw.err" \
    || fail "a middleware-answered response did not keep the connection open"
grep -q "Re-using existing connection" "$TMPDIR/mw.err" \
    || fail "the second middleware request opened a new connection"

echo "  [PASS] http_head_and_middleware: HEAD sends no body, falls back to GET, and a middleware answer keeps the connection"
