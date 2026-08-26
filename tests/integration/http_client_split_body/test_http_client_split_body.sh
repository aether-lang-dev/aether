#!/bin/sh
# Integration test: std.http.client reads to the end of the response, not to
# the end of the headers.
#
# Response framing (#1653) decides where one response ends so the connection
# can carry the next. Every other client test sends a small response that
# arrives in a single segment, so a client that stopped at the header block
# would still pass them. This upstream sends the headers, pauses, then sends
# the body, which fails any client that does not honour Content-Length.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] http_client_split_body on Windows (raw POSIX sockets)"
        exit 0
        ;;
esac

if [ ! -x "$AE" ]; then
    echo "  [SKIP] http_client_split_body: ae not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
UP_PID=""
cleanup() { [ -n "$UP_PID" ] && kill "$UP_PID" 2>/dev/null; rm -rf "$TMPDIR"; }
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

cc "$SCRIPT_DIR/split_body_upstream.c" -o "$TMPDIR/upstream" 2>"$TMPDIR/cc.log" \
    || { cat "$TMPDIR/cc.log"; fail "could not compile split_body_upstream.c"; }
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

OUT="$(UPSTREAM_PORT="$PORT" AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/split_body.ae" 2>&1)"
if ! echo "$OUT" | grep -q "^PASS"; then
    echo "$OUT"
    fail "the client did not read the body that arrived after the headers"
fi

echo "  [PASS] http_client_split_body: the full body was read across a split response"
