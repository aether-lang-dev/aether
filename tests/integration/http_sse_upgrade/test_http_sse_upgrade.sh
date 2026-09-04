#!/bin/sh
# Upgrade-in-place SSE: response_upgrade_sse + the `retry:` field.
#
# `server_sse` registers a whole ROUTE as SSE, so the decision is made before
# the request is parsed. A handler that must answer 400 on a malformed body and
# only THEN stream cannot use it -- which is why the Datastar SDK bypassed the
# stdlib entirely and seized the raw socket with response_accept_tunnel. That
# works, but accept_tunnel returns NULL on a TLS connection, so every SSE
# endpoint built that way is plaintext-only.
#
# Asserts:
#   - a handler can answer 400 WITHOUT upgrading (the reason for the seam)
#   - an upgraded stream carries the fixed SSE head
#   - `retry:` is emitted, in spec order (id, event, retry, data). 5 of the 20
#     Datastar cross-SDK conformance goldens require it, and it is SSE's own
#     reconnection field, not a Datastar invention
#   - upgrading over an already-committed body is REFUSED, so that body is not
#     silently lost
#
# Skips on Windows for the same reason http_server_sse does: this is
# platform-independent userland C and each curl under MSYS2 costs 10-100x a
# POSIX spawn.
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_sse_upgrade — HTTP server code is platform-independent; covered by POSIX matrix"
        exit 0
        ;;
esac

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
command -v curl >/dev/null 2>&1 || { echo "  [SKIP] curl not on PATH"; exit 0; }

TMPDIR="$(mktemp -d)"
cleanup() {
    if [ -n "${SRV_PID:-}" ]; then
        kill "$SRV_PID" 2>/dev/null || true
        wait "$SRV_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!
PORT=18131

# Probe the port rather than trusting READY: server.ae prints it before the
# StartSrv message opens the listener, and a curl issued too early returns
# nothing at all.
deadline=$(($(date +%s) + 15))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        echo "  [FAIL] http_sse_upgrade: server died:"; head -20 "$TMPDIR/srv.log"; exit 1
    fi
    curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$PORT/stream?bad" 2>/dev/null && break
    sleep 0.1
done

# --- 1. the 400-first path: answer WITHOUT upgrading --------------------
CODE=$(curl -s -o "$TMPDIR/bad.out" -w "%{http_code}" --max-time 5 \
       "http://127.0.0.1:$PORT/stream?bad" 2>/dev/null || echo 000)
[ "$CODE" = "400" ] || {
    echo "  [FAIL] http_sse_upgrade: bad request returned $CODE, want 400"; exit 1; }

# --- 2. the stream: headers -------------------------------------------
curl -sN -D "$TMPDIR/h" -o "$TMPDIR/sse.out" --max-time 5 \
     "http://127.0.0.1:$PORT/stream" 2>/dev/null || true
grep -qi "^Content-Type: *text/event-stream" "$TMPDIR/h" || {
    echo "  [FAIL] http_sse_upgrade: no text/event-stream on the upgraded response"
    sed 's/^/    /' "$TMPDIR/h" | head -8; exit 1; }

# --- 3. retry:, and the field ORDER the spec requires ------------------
grep -q "^retry: 2000$" "$TMPDIR/sse.out" || {
    echo "  [FAIL] http_sse_upgrade: no 'retry: 2000' line"
    sed 's/^/    /' "$TMPDIR/sse.out" | head -12; exit 1; }
# retry must PRECEDE the data lines: the blank line dispatches the event, so a
# field after the data would land in the next event (or be dropped).
RETRY_AT=$(grep -n "^retry: 2000$" "$TMPDIR/sse.out" | head -1 | cut -d: -f1)
DATA_AT=$(grep -n "^data: selector div$" "$TMPDIR/sse.out" | head -1 | cut -d: -f1)
[ -n "$RETRY_AT" ] && [ -n "$DATA_AT" ] && [ "$RETRY_AT" -lt "$DATA_AT" ] || {
    echo "  [FAIL] http_sse_upgrade: retry: must precede the data lines (retry@$RETRY_AT data@$DATA_AT)"
    sed 's/^/    /' "$TMPDIR/sse.out" | head -12; exit 1; }
# and the plain event still works, with no stray retry on it
grep -q "^event: greet$" "$TMPDIR/sse.out" || {
    echo "  [FAIL] http_sse_upgrade: the plain sse_send event is missing"; exit 1; }

# --- 4. upgrading over a committed body must be REFUSED ----------------
BODY=$(curl -s --max-time 5 "http://127.0.0.1:$PORT/stream?hasbody" 2>/dev/null || echo "")
case "$BODY" in
    *BUG-upgraded-over-body*)
        echo "  [FAIL] http_sse_upgrade: upgraded over an already-committed body"; exit 1 ;;
    *already\ answered*) ;;
    *)
        echo "  [FAIL] http_sse_upgrade: unexpected body for ?hasbody: '$BODY'"; exit 1 ;;
esac

echo "  [PASS] http_sse_upgrade: 400-then-upgrade, retry: in spec order, refuses over a body"
