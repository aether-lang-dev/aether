#!/bin/sh
# Issue #383 — zero-copy sendfile(2) fast path for http_serve_file.
#
# Verifies:
#   1. Cleartext HTTP/1.1 GET of a 1 MiB binary file produces a
#      byte-identical body. Sendfile path is the one used.
#   2. Two requests over a single keep-alive connection both succeed
#      (Content-Length matches sendfile bytes; otherwise the second
#      request would read garbage from the leftover body).
#   3. Range request → fallback to buffered path; response is still
#      200 with full body (Range slicing is v2; falling back is the
#      documented behaviour).
#   4. 404 path doesn't leak FD (server still serves further
#      requests; verified by a follow-up GET).

# Skip on Windows — the HTTP server / proxy / middleware code path
# under test is platform-independent userland C; the Linux and
# macOS CI matrix entries already exercise every behaviour this
# test asserts. Each curl invocation under MSYS2 bash pays Cygwin
# fork-emulation + Defender + slower-than-POSIX localhost overhead
# (~10-100x POSIX's per-spawn cost), so running these on Windows
# adds minutes to the pipeline without adding coverage. Windows-
# specific runtime concerns (file I/O, process spawning, path
# handling) are covered by fs_*, std_ipc_*, run_lib_path, and
# ae_help tests respectively.
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_sendfile — HTTP server/proxy/middleware code is platform-independent; covered by POSIX matrix"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v curl >/dev/null 2>&1; then
    echo "  [SKIP] curl not on PATH"; exit 0
fi

TMPDIR="$(mktemp -d)"
SRV_PID=""
cleanup() {
    if [ -n "$SRV_PID" ]; then
        kill "$SRV_PID" 2>/dev/null || true
        wait "$SRV_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

# Build the server binary.
if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/server.ae" \
        -o "$TMPDIR/server" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] build:"; head -30 "$TMPDIR/build.log"; exit 1
fi

# Generate the static-file root + 1 MiB binary asset.
ROOT_DIR="$TMPDIR/static_root"
mkdir -p "$ROOT_DIR"
# Deterministic 1 MiB content via /dev/urandom into a fixed path.
dd if=/dev/urandom of="$ROOT_DIR/blob.bin" bs=1024 count=1024 \
   >/dev/null 2>&1

# Boot the server.
"$TMPDIR/server" "$TMPDIR/server" "$ROOT_DIR" >"$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

# Wait for READY.
deadline=$(($(date +%s) + 30))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if grep -q '^READY' "$TMPDIR/srv.log" 2>/dev/null; then break; fi
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        echo "  [FAIL] server died:"; head -20 "$TMPDIR/srv.log"; exit 1
    fi
    sleep 0.1
done
sleep 0.3

URL="http://127.0.0.1:19200"

# --- Test 1: byte-identical body over cleartext h1.1 ---
RESP_BODY="$TMPDIR/got1.bin"
HTTP_CODE=$(curl --silent --show-error --max-time 10 \
                 -o "$RESP_BODY" -w '%{http_code}' \
                 "$URL/static/blob.bin" 2>"$TMPDIR/c1.err") || {
    echo "  [FAIL] T1 curl:"; cat "$TMPDIR/c1.err"; exit 1
}
[ "$HTTP_CODE" = "200" ] || { echo "  [FAIL] T1 status: $HTTP_CODE"; exit 1; }
SENT_BYTES=$(wc -c <"$ROOT_DIR/blob.bin" | tr -d ' ')
RECV_BYTES=$(wc -c <"$RESP_BODY" | tr -d ' ')
[ "$SENT_BYTES" = "$RECV_BYTES" ] || {
    echo "  [FAIL] T1 length: sent=$SENT_BYTES got=$RECV_BYTES"; exit 1;
}
SENT_HEX=$(od -An -tx1 -v "$ROOT_DIR/blob.bin" | tr -d ' \n')
RECV_HEX=$(od -An -tx1 -v "$RESP_BODY" | tr -d ' \n')
[ "$SENT_HEX" = "$RECV_HEX" ] || {
    echo "  [FAIL] T1 content mismatch (1 MiB body)"; exit 1;
}

# --- Test 2: keep-alive — two requests, second isn't corrupted ---
# Single curl invocation over -K config drives two requests on the
# same TCP connection (default for HTTP/1.1). The second response
# is what we verify; if Content-Length mismatched the body length,
# its parser would either hang or read junk.
KA1="$TMPDIR/ka1.bin"
KA2="$TMPDIR/ka2.bin"
curl --silent --show-error --max-time 15 \
     -o "$KA1" "$URL/static/blob.bin" \
     -o "$KA2" "$URL/static/blob.bin" 2>"$TMPDIR/c2.err" || {
    echo "  [FAIL] T2 curl:"; cat "$TMPDIR/c2.err"; exit 1
}
KA2_HEX=$(od -An -tx1 -v "$KA2" | tr -d ' \n')
[ "$KA2_HEX" = "$SENT_HEX" ] || {
    echo "  [FAIL] T2 keep-alive: second response corrupted"; exit 1;
}

# --- Test 3: Range request → fallback to buffered (200 + full body) ---
RNG_BODY="$TMPDIR/range.bin"
HTTP_CODE=$(curl --silent --show-error --max-time 10 \
                 -H 'Range: bytes=0-1023' \
                 -o "$RNG_BODY" -w '%{http_code}' \
                 "$URL/static/blob.bin" 2>"$TMPDIR/c3.err") || {
    echo "  [FAIL] T3 curl:"; cat "$TMPDIR/c3.err"; exit 1
}
[ "$HTTP_CODE" = "200" ] || { echo "  [FAIL] T3 status: $HTTP_CODE (expected 200 — Range falls back)"; exit 1; }
RNG_HEX=$(od -An -tx1 -v "$RNG_BODY" | tr -d ' \n')
[ "$RNG_HEX" = "$SENT_HEX" ] || {
    echo "  [FAIL] T3 Range fallback content mismatch"; exit 1;
}

# --- Test 5: a SMALL file must arrive without waiting for the close ---
# T2 uses the 1 MiB blob, which is larger than the socket buffer, so the
# kernel has to push it whatever the sendfile path does with TCP_CORK /
# TCP_NOPUSH. A small file fits, and then nothing forces the push: on Darwin
# and the BSDs, clearing TCP_NOPUSH does not send what is queued, so the
# response reached the client only when the connection closed. That is
# invisible while a worker goes straight back to recv() on the socket, and a
# hung request once idle connections are parked instead (#1663).
#
# Two requests on one connection is what pins it: curl cannot send the second
# until the first response arrives, so a response held back stalls here rather
# than being papered over by the close at the end of the test.
printf 'small-file-body\n' > "$ROOT_DIR/small.txt"
SM1="$TMPDIR/sm1.txt"
SM2="$TMPDIR/sm2.txt"
curl --silent --show-error --max-time 5 \
     -o "$SM1" "$URL/static/small.txt" \
     -o "$SM2" "$URL/static/small.txt" 2>"$TMPDIR/c5.err" || {
    echo "  [FAIL] T5 small file over keep-alive did not complete within 5s:"
    cat "$TMPDIR/c5.err"
    echo "         (a sendfile response that only arrives when the connection"
    echo "          closes looks exactly like this)"
    exit 1
}
[ "$(cat "$SM1")" = "small-file-body" ] || {
    echo "  [FAIL] T5 first response: $(cat "$SM1")"; exit 1;
}
[ "$(cat "$SM2")" = "small-file-body" ] || {
    echo "  [FAIL] T5 second response: $(cat "$SM2")"; exit 1;
}

# --- Test 4: 404 doesn't break further requests (no FD leak) ---
HTTP_CODE=$(curl --silent --show-error --max-time 5 \
                 -o /dev/null -w '%{http_code}' \
                 "$URL/static/missing.bin" 2>"$TMPDIR/c4.err") || true
[ "$HTTP_CODE" = "404" ] || { echo "  [FAIL] T4 missing: $HTTP_CODE"; exit 1; }

# Server still serves the existing file after the 404.
HTTP_CODE=$(curl --silent --show-error --max-time 5 \
                 -o /dev/null -w '%{http_code}' \
                 "$URL/static/blob.bin" 2>"$TMPDIR/c4b.err") || true
[ "$HTTP_CODE" = "200" ] || { echo "  [FAIL] T4 follow-up: $HTTP_CODE"; exit 1; }

echo "  [PASS] http_sendfile: 5/5 — body / keep-alive / Range fallback / small-file flush / 404 cleanup"
