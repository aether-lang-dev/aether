#!/bin/sh
# #626 (upload half) — RAM-bounded streaming request body.
#
# PUTs a multi-megabyte body (well over the 16 KiB streaming threshold,
# so the server takes the streaming path and never buffers it whole),
# and asserts:
#   1. the server streamed every byte (received == file size)
#   2. byte-identical round-trip (server's SHA-256 of what it streamed
#      to disk == SHA-256 of the file we sent)
#   3. the keep-alive connection boundary stays clean afterward — a
#      follow-up GET /ping on the same connection returns "pong"
#      (proves the post-handler drain left read_pos at the next request)
#
# Runs on Windows too: the server streams the body to a file through
# fs.open + pwrite, and that path was Windows-specific in the worst way —
# fs.open handed the caller's "w" to fopen as a TEXT mode, so every '\n'
# in the 3 MiB body became "\r\n" on disk and the digest never matched.
# The skip that used to sit here ("covered by POSIX matrix") hid it.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
AE="$ROOT/build/ae"

command -v curl >/dev/null 2>&1 || { echo "  [SKIP] curl not on PATH"; exit 0; }
command -v sha256sum >/dev/null 2>&1 && SHA=sha256sum || SHA="shasum -a 256"

TMPDIR="$(mktemp -d)"
SRV_PID=""
cleanup() {
    [ -n "$SRV_PID" ] && { kill "$SRV_PID" 2>/dev/null || true; wait "$SRV_PID" 2>/dev/null || true; }
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

# 3 MiB of random bytes — ~192 streaming windows at 64 KiB each, and far
# beyond anything that fits in the 16 KiB connection buffer.
PAYLOAD="$TMPDIR/payload.bin"
head -c 3145728 /dev/urandom > "$PAYLOAD"
WANT_SHA=$($SHA "$PAYLOAD" | awk '{print $1}')
WANT_LEN=$(wc -c < "$PAYLOAD" | tr -d ' ')

if ! "$AE" build "$SCRIPT_DIR/server.ae" -o "$TMPDIR/server" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] http_stream_upload: server build failed"
    sed 's/^/    /' "$TMPDIR/build.log" | head -20
    exit 1
fi

AETHER_HOME="$ROOT" "$TMPDIR/server" "$TMPDIR" >"$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

deadline=$(($(date +%s) + 15))
while [ "$(date +%s)" -lt "$deadline" ]; do
    grep -q READY "$TMPDIR/srv.log" 2>/dev/null && break
    kill -0 "$SRV_PID" 2>/dev/null || { echo "  [FAIL] server died:"; head -30 "$TMPDIR/srv.log"; exit 1; }
    sleep 0.1
done
PORT=$(read_ready_port "$TMPDIR/srv.log") || exit 1
wait_port "$PORT" || exit 1

# Single curl invocation: PUT the big body, then GET /ping on the SAME
# connection (curl reuses the keep-alive socket for sequential URLs).
RESP=$(curl --silent --show-error --max-time 30 \
            --data-binary "@$PAYLOAD" \
            -H "Content-Type: application/octet-stream" \
            -X POST "http://127.0.0.1:$PORT/upload" \
            --next "http://127.0.0.1:$PORT/ping" \
            2>"$TMPDIR/curl.err") || {
    echo "  [FAIL] curl failed:"; cat "$TMPDIR/curl.err"; head -20 "$TMPDIR/srv.log"; exit 1
}

# RESP is the upload reply concatenated with the ping reply ("pong").
# Pull the fields out of the upload reply.
if echo "$RESP" | grep -q "sha256=unavailable"; then
    echo "  [SKIP] http_stream_upload — server built without OpenSSL (no digest)"
    exit 0
fi

GOT_LEN=$(echo "$RESP" | sed -n 's/.*received=\([0-9]*\).*/\1/p' | head -1)
GOT_SHA=$(echo "$RESP" | sed -n 's/.*sha256=\([0-9a-f]*\).*/\1/p' | head -1)

if [ "$GOT_LEN" != "$WANT_LEN" ]; then
    echo "  [FAIL] streamed byte count: got '$GOT_LEN', want '$WANT_LEN'"; exit 1
fi
if [ "$GOT_SHA" != "$WANT_SHA" ]; then
    echo "  [FAIL] body digest mismatch:"
    echo "    server streamed sha256=$GOT_SHA"
    echo "    sent file     sha256=$WANT_SHA"
    exit 1
fi
if ! echo "$RESP" | grep -q "pong"; then
    echo "  [FAIL] keep-alive boundary dirty — GET /ping after upload did not return pong:"
    echo "    full response: $RESP"
    exit 1
fi

# #644 — request_body_complete around the streaming loop: not complete at
# handler entry (dispatched at headers-complete), complete after the drain.
if ! echo "$RESP" | grep -q "pre=0"; then
    echo "  [FAIL] body_complete at handler entry should be 0 on a streaming request: $RESP"
    exit 1
fi
if ! echo "$RESP" | grep -q "post=1"; then
    echo "  [FAIL] body_complete after the read loop should be 1: $RESP"
    exit 1
fi

# #644 v1 contract — the whole-body accessor on the SAME large payload must
# materialize on demand and return every byte (pre-fix it returned "" for
# any body over the 16 KiB streaming threshold). Keep-alive after the
# materializing route must stay clean too (drain no-ops on a consumed body).
RESP2=$(curl --silent --show-error --max-time 30 \
            --data-binary "@$PAYLOAD" \
            -H "Content-Type: application/octet-stream" \
            -X POST "http://127.0.0.1:$PORT/v1body" \
            --next "http://127.0.0.1:$PORT/ping" \
            2>"$TMPDIR/curl2.err") || {
    echo "  [FAIL] v1body curl failed:"; cat "$TMPDIR/curl2.err"; head -20 "$TMPDIR/srv.log"; exit 1
}
if echo "$RESP2" | grep -q "v1sha=unavailable"; then
    echo "  [SKIP] http_stream_upload v1body — server built without OpenSSL (no digest)"
    exit 0
fi
V1_LEN=$(echo "$RESP2" | sed -n 's/.*v1len=\([0-9]*\).*/\1/p' | head -1)
V1_SHA=$(echo "$RESP2" | sed -n 's/.*v1sha=\([0-9a-f]*\).*/\1/p' | head -1)
if [ "$V1_LEN" != "$WANT_LEN" ]; then
    echo "  [FAIL] v1 request_body length: got '$V1_LEN', want '$WANT_LEN' (materialize-on-demand broken)"; exit 1
fi
if [ "$V1_SHA" != "$WANT_SHA" ]; then
    echo "  [FAIL] v1 request_body digest mismatch (materialized body corrupt):"
    echo "    server sha256=$V1_SHA"
    echo "    sent   sha256=$WANT_SHA"
    exit 1
fi
if ! echo "$RESP2" | grep -q "v1pre=0"; then
    echo "  [FAIL] v1body: body_complete at entry should be 0 on a streaming request: $RESP2"; exit 1
fi
if ! echo "$RESP2" | grep -q "v1post=1"; then
    echo "  [FAIL] v1body: body_complete after materialization should be 1: $RESP2"; exit 1
fi
if ! echo "$RESP2" | grep -q "pong"; then
    echo "  [FAIL] keep-alive boundary dirty after materializing route: $RESP2"; exit 1
fi

# Small (buffered, < 16 KiB) body: complete at entry by construction.
SMALL="$TMPDIR/small.bin"
head -c 512 /dev/urandom > "$SMALL"
SMALL_LEN=$(wc -c < "$SMALL" | tr -d ' ')
RESP3=$(curl --silent --show-error --max-time 10 \
            --data-binary "@$SMALL" \
            -H "Content-Type: application/octet-stream" \
            -X POST "http://127.0.0.1:$PORT/v1body" \
            2>"$TMPDIR/curl3.err") || {
    echo "  [FAIL] small v1body curl failed:"; cat "$TMPDIR/curl3.err"; exit 1
}
S_LEN=$(echo "$RESP3" | sed -n 's/.*v1len=\([0-9]*\).*/\1/p' | head -1)
if [ "$S_LEN" != "$SMALL_LEN" ]; then
    echo "  [FAIL] small v1body length: got '$S_LEN', want '$SMALL_LEN': $RESP3"; exit 1
fi
if ! echo "$RESP3" | grep -q "v1pre=1"; then
    echo "  [FAIL] small (buffered) body should be complete at handler entry: $RESP3"; exit 1
fi

echo "  [PASS] http_stream_upload: 3 MiB streamed (bounded), byte-identical, keep-alive clean, v1 whole-body materializes, body_complete correct"
