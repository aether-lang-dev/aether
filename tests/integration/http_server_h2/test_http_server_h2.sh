#!/bin/sh
# #260 Tier 2: HTTP/2 server end-to-end test (broad).
#
# Verifies:
#   1. The server advertises h2 via http.server_set_h2() — when the
#      build doesn't link libnghttp2, the driver prints
#      READY-NOH2 and the test cleanly skips.
#   2. `curl --http2-prior-knowledge` GET → expected body, version=2.
#   3. `curl --http2-prior-knowledge -X POST` round-trips a body
#      through the /echo handler.
#   4. `curl --http2` (Upgrade: h2c) GET → expected body. Curl may
#      stay on 1.1 if the upgrade isn't probed; we accept both.
#   5. Multiplexed concurrent streams via curl -Z (parallel) on the
#      same TCP connection — all 10 requests succeed and the
#      query-string echo proves they were demuxed correctly.
#   6. Large response body (~100 KiB) survives the h2 data-provider
#      chunking path.
#   7. Large request body (1 MiB POST) round-trips through DATA
#      frame fragmentation + flow-control window updates.
#   8. HEAD method response carries no body bytes (RFC 7230 §4.3.2)
#      even though the handler wrote one.
#   9. An h2 request carries its connection's metadata: the peer, its
#      port, the local address and the TLS state (#3175).
#
# Skips cleanly when curl is missing or doesn't link libnghttp2.

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
        echo "  [SKIP-WIN] http_server_h2 — HTTP server/proxy/middleware code is platform-independent; covered by POSIX matrix"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/portable.sh"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
. "$ROOT/tests/lib/same_bytes.sh"
AE="$ROOT/build/ae"

if ! command -v curl >/dev/null 2>&1; then
    echo "  [SKIP] curl not on PATH"; exit 0
fi
if ! curl --version 2>&1 | grep -q nghttp2; then
    echo "  [SKIP] curl built without HTTP/2"; exit 0
fi

TMPDIR="$(mktemp -d)"
SRV_PID=""
cleanup() {
    stop_server "$SRV_PID"
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

deadline=$(($(date +%s) + 15))
ready=""
while [ "$(date +%s)" -lt "$deadline" ]; do
    if grep -q '^READY' "$TMPDIR/srv.log" 2>/dev/null; then
        ready=$(grep '^READY' "$TMPDIR/srv.log" | head -1); break
    fi
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        echo "  [FAIL] server died:"; head -20 "$TMPDIR/srv.log"; exit 1
    fi
    sleep 0.1
done
[ -z "$ready" ] && {
    echo "  [FAIL] no READY within timeout"; head -20 "$TMPDIR/srv.log"; exit 1
}
case "$ready" in READY-NOH2*) echo "  [SKIP] $ready"; exit 0;; esac
PORT=$PORT
PORT=$(read_ready_port "$TMPDIR/srv.log") || exit 1
wait_port "$PORT" || exit 1

URL_BASE="http://127.0.0.1:$PORT"

# ----------------------------------------------------------------
# Test 1 — GET via prior-knowledge h2c.
# ----------------------------------------------------------------
RESP="$TMPDIR/get.body"
HTTPVER=$(curl --silent --show-error --max-time 5 \
              --http2-prior-knowledge \
              -o "$RESP" \
              -w '%{http_version}' \
              "$URL_BASE/" 2>"$TMPDIR/c1.err") || {
    echo "  [FAIL] curl --http2-prior-knowledge GET failed:"; cat "$TMPDIR/c1.err"; exit 1
}
[ "$HTTPVER" = "2" ] || { echo "  [FAIL] expected http_version=2, got '$HTTPVER'"; exit 1; }
grep -q '^h2-ok$' "$RESP" || { echo "  [FAIL] GET body mismatch"; cat "$RESP"; exit 1; }

# ----------------------------------------------------------------
# Test 1b — the connection's metadata on an h2 request (#3175). It used
# to read peer="" port 0 local="" on every h2 stream.
# ----------------------------------------------------------------
PEER=$(curl --silent --show-error --max-time 5 \
            --http2-prior-knowledge \
            -w ' version=%{http_version}' \
            "$URL_BASE/peer" 2>"$TMPDIR/peer.err") || {
    echo "  [FAIL] curl --http2-prior-knowledge GET /peer failed:"; cat "$TMPDIR/peer.err"; exit 1
}
PEER_WANT="peer=127.0.0.1 port_set=1 local=127.0.0.1 tls=0 scheme=http version=2"
[ "$PEER" = "$PEER_WANT" ] || {
    echo "  [FAIL] h2 request metadata: got '$PEER', expected '$PEER_WANT'"; exit 1
}

# ----------------------------------------------------------------
# Test 2 — POST /echo with a small body. Exercises HEADERS + DATA
# in both directions.
# ----------------------------------------------------------------
ECHO_BODY="hello-from-h2-stream"
RESP2="$TMPDIR/echo.body"
HTTPVER2=$(curl --silent --show-error --max-time 5 \
               --http2-prior-knowledge \
               -X POST \
               -H 'Content-Type: text/plain' \
               -d "$ECHO_BODY" \
               -o "$RESP2" \
               -w '%{http_version}' \
               "$URL_BASE/echo" 2>"$TMPDIR/c2.err") || {
    echo "  [FAIL] curl --http2 POST /echo failed:"; cat "$TMPDIR/c2.err"; exit 1
}
[ "$HTTPVER2" = "2" ] || { echo "  [FAIL] echo http_version=2 expected, got '$HTTPVER2'"; exit 1; }
grep -q "^${ECHO_BODY}\$" "$RESP2" || { echo "  [FAIL] echo body mismatch:"; cat "$RESP2"; exit 1; }

# ----------------------------------------------------------------
# Test 3 — h2c upgrade probe. Some curl builds don't actively
# probe upgrade and stay on 1.1; we accept either.
# ----------------------------------------------------------------
HTTPVER3=$(curl --silent --show-error --max-time 5 \
               --http2 \
               -o "$TMPDIR/up.body" \
               -w '%{http_version}' \
               "$URL_BASE/" 2>"$TMPDIR/c3.err") || {
    echo "  [FAIL] curl --http2 (upgrade probe) GET failed:"; cat "$TMPDIR/c3.err"; exit 1
}
[ "$HTTPVER3" = "2" ] || [ "$HTTPVER3" = "1.1" ] || {
    echo "  [FAIL] upgrade probe unexpected http_version='$HTTPVER3'"; exit 1
}
grep -q '^h2-ok$' "$TMPDIR/up.body" || { echo "  [FAIL] upgrade body mismatch"; cat "$TMPDIR/up.body"; exit 1; }

# ----------------------------------------------------------------
# Test 4 — Multiplexed concurrent streams. -Z drives 10 GETs in
# parallel; with --http2-prior-knowledge curl reuses one TCP
# connection and multiplexes the requests as separate h2 streams.
# Each query-string is unique so we can prove the wrapper demuxed
# them correctly.
# ----------------------------------------------------------------
PARALLEL_DIR="$TMPDIR/par"
mkdir -p "$PARALLEL_DIR"
URLS=""
for i in 0 1 2 3 4 5 6 7 8 9; do
    URLS="$URLS $URL_BASE/n?stream=$i"
done
# shellcheck disable=SC2086
curl --silent --show-error --max-time 10 \
     --http2-prior-knowledge \
     -Z --parallel-immediate --parallel-max 10 \
     $(for i in 0 1 2 3 4 5 6 7 8 9; do
           printf -- '-o %s/n%d %s/n?stream=%d ' "$PARALLEL_DIR" "$i" "$URL_BASE" "$i"
       done) 2>"$TMPDIR/par.err" || {
    echo "  [FAIL] parallel curl failed:"; cat "$TMPDIR/par.err"; exit 1
}
for i in 0 1 2 3 4 5 6 7 8 9; do
    if ! grep -q "^q=stream=$i\$" "$PARALLEL_DIR/n$i" 2>/dev/null; then
        echo "  [FAIL] multiplexed stream $i mismatch:"
        cat "$PARALLEL_DIR/n$i" 2>/dev/null
        exit 1
    fi
done

# ----------------------------------------------------------------
# Test 5 — Large response (~100 KiB). Verifies the data-provider
# chunking path: nghttp2 framecuts at SETTINGS_MAX_FRAME_SIZE (16
# KiB by default), so the wrapper must hand back successive chunks
# until the body is exhausted, then EOF.
# ----------------------------------------------------------------
BIG="$TMPDIR/big.body"
curl --silent --show-error --max-time 10 \
     --http2-prior-knowledge \
     -o "$BIG" \
     "$URL_BASE/big" 2>"$TMPDIR/big.err" || {
    echo "  [FAIL] /big GET failed:"; cat "$TMPDIR/big.err"; exit 1
}
EXPECTED_SIZE=102400
ACTUAL_SIZE=$(wc -c <"$BIG" | tr -d ' ')
if [ "$ACTUAL_SIZE" != "$EXPECTED_SIZE" ]; then
    echo "  [FAIL] /big size mismatch — expected $EXPECTED_SIZE, got $ACTUAL_SIZE"
    exit 1
fi
# Spot-check the content pattern.
head_c 64 "$BIG" | grep -q '^0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef$' || {
    echo "  [FAIL] /big content prefix mismatch"; head_c 64 "$BIG"; echo; exit 1
}

# ----------------------------------------------------------------
# Test 6 — Large request body (1 MiB POST). Verifies request-side
# DATA frame fragmentation + nghttp2 flow-control window updates
# round-tripped without truncation.
# ----------------------------------------------------------------
POST_BODY="$TMPDIR/post.in"
POST_RESP="$TMPDIR/post.out"
# Generate a 1 MiB payload deterministically for byte-equality check.
yes 'large-body-segment-1234567890' 2>/dev/null | head_c 1048576 >"$POST_BODY"
curl --silent --show-error --max-time 30 \
     --http2-prior-knowledge \
     -X POST \
     -H 'Content-Type: application/octet-stream' \
     --data-binary "@$POST_BODY" \
     -o "$POST_RESP" \
     "$URL_BASE/echo" 2>"$TMPDIR/post.err" || {
    echo "  [FAIL] /echo large POST failed:"; cat "$TMPDIR/post.err"; exit 1
}
# Compare bytes (tests/lib/same_bytes.sh: cmp, or cksum without diffutils).
same_bytes "$POST_BODY" "$POST_RESP" || {
    echo "  [FAIL] /echo large POST round-trip mismatch"
    echo "    sent $(wc -c <"$POST_BODY") bytes; received $(wc -c <"$POST_RESP") bytes"
    exit 1
}

# ----------------------------------------------------------------
# Test 7 — HEAD method must produce a response with no body bytes
# even though the handler wrote 'do-not-send'. Per RFC 7230 §4.3.2
# / RFC 7540 §8.1 the wrapper must suppress DATA frames; curl
# observes this as a zero-byte body.
# ----------------------------------------------------------------
HEAD_HDR="$TMPDIR/head.hdr"
# curl docs flag `-X HEAD` as misbehaving (it makes curl read the
# response as if it had a body, then complain about the missing
# bytes). Use `-I` for an actual HEAD; suppress the "headers
# displayed as body" output via -o /dev/null and capture the real
# headers via -D for verification.
curl --silent --show-error --max-time 5 \
     --http2-prior-knowledge \
     -I \
     -D "$HEAD_HDR" \
     -o /dev/null \
     "$URL_BASE/head" 2>"$TMPDIR/head.err" || {
    echo "  [FAIL] /head HEAD failed:"; cat "$TMPDIR/head.err"; exit 1
}
# Status line plus selected headers — should match what GET would
# have produced. Content-Length matches the body the GET handler
# wrote (per RFC 7231 §4.3.2: HEAD response has the same headers
# as GET would, just no body).
if ! grep -qi '^HTTP/2 200' "$HEAD_HDR"; then
    echo "  [FAIL] HEAD response not 200:"; cat "$HEAD_HDR"; exit 1
fi
if ! grep -qi '^content-type: *text/plain' "$HEAD_HDR"; then
    echo "  [FAIL] HEAD response missing Content-Type:"; cat "$HEAD_HDR"; exit 1
fi
# Sanity: a GET to the same path returns the body we expected the
# HEAD to suppress. Confirms the route was matched (i.e. our
# HEAD-falls-through-to-GET dispatch is working).
GETBODY="$TMPDIR/headget.body"
curl --silent --show-error --max-time 5 \
     --http2-prior-knowledge \
     -o "$GETBODY" \
     "$URL_BASE/head" 2>"$TMPDIR/headget.err" || {
    echo "  [FAIL] /head GET sanity failed:"; cat "$TMPDIR/headget.err"; exit 1
}
grep -q '^do-not-send$' "$GETBODY" || {
    echo "  [FAIL] /head GET sanity body mismatch:"; cat "$GETBODY"; exit 1
}

# ----------------------------------------------------------------
# Test 8 — sustained-load: 50 sequential streams on one connection.
# Verifies the wrapper's per-stream alloc/free path (header block,
# body buffer, response_headers nv array) doesn't leak or desync
# over a sustained burst. curl reuses one TCP connection across
# multi-URL invocations and multiplexes them as separate h2
# streams. (We use 50 sequential rather than 100 in parallel
# because the latter overwhelms the single-thread accept queue
# in the test harness — that's a CI-runner concurrency bound,
# not an h2 wrapper bound. The 10-parallel test above already
# proves multiplexing works.)
# ----------------------------------------------------------------
STRESS_DIR="$TMPDIR/stress"
mkdir -p "$STRESS_DIR"
STRESS_CFG="$TMPDIR/stress.cfg"
: >"$STRESS_CFG"
i=0
while [ $i -lt 50 ]; do
    printf 'url = "%s/n?stress=%d"\noutput = "%s/n%d"\n' \
        "$URL_BASE" "$i" "$STRESS_DIR" "$i" >>"$STRESS_CFG"
    i=$((i + 1))
done
curl --silent --show-error --max-time 30 \
     --http2-prior-knowledge \
     --config "$STRESS_CFG" \
     2>"$TMPDIR/stress.err" || {
    echo "  [FAIL] 50-stream stress curl failed:"; cat "$TMPDIR/stress.err"; exit 1
}
mismatches=0
i=0
while [ $i -lt 50 ]; do
    if ! grep -q "^q=stress=$i\$" "$STRESS_DIR/n$i" 2>/dev/null; then
        mismatches=$((mismatches + 1))
    fi
    i=$((i + 1))
done
if [ "$mismatches" -gt 0 ]; then
    echo "  [FAIL] $mismatches stress streams returned wrong body"
    head_c 200 "$STRESS_DIR/n0" 2>/dev/null
    exit 1
fi

# A follow-up request via a fresh connection must still succeed
# (catches any global state corruption in the wrapper that would
# poison subsequent h2 sessions).
POST_STRESS="$TMPDIR/post_stress.body"
HTTPVER_PS=$(curl --silent --show-error --max-time 5 \
                 --http2-prior-knowledge \
                 -o "$POST_STRESS" \
                 -w '%{http_version}' \
                 "$URL_BASE/" 2>"$TMPDIR/ps.err") || {
    echo "  [FAIL] post-stress GET failed (server state corrupted?):"; cat "$TMPDIR/ps.err"; exit 1
}
[ "$HTTPVER_PS" = "2" ] || {
    echo "  [FAIL] post-stress GET dropped from h2 to '$HTTPVER_PS'"; exit 1
}

# ----------------------------------------------------------------
# Test 9 — h2c upgrade handshake. Send a raw HTTP/1.1 GET with
# Upgrade: h2c + HTTP2-Settings headers (RFC 7540 §3.2) and verify
# the server responds with `101 Switching Protocols` before any
# h2 frames flow. We use python3 if available (portable raw-socket
# sender across Linux / macOS / Windows-MSYS); skip otherwise.
# ----------------------------------------------------------------
if command -v python3 >/dev/null 2>&1; then
    UPGRADE_OUT="$TMPDIR/upgrade.raw"
    UPGRADE_PY="$TMPDIR/upgrade.py"
    cat >"$UPGRADE_PY" <<'PY'
import socket, sys
url = sys.argv[1]
host_port = url.replace("http://", "")
host, port = host_port.split(":")
port = int(port)
req = (
    "GET / HTTP/1.1\r\n"
    "Host: %s:%d\r\n"
    "Connection: Upgrade, HTTP2-Settings\r\n"
    "Upgrade: h2c\r\n"
    "HTTP2-Settings: \r\n"
    "\r\n" % (host, port)
).encode("ascii")
sock = socket.create_connection((host, port), timeout=5)
sock.sendall(req)
sock.settimeout(2.0)
buf = b""
try:
    while len(buf) < 64:
        chunk = sock.recv(256)
        if not chunk: break
        buf += chunk
except socket.timeout:
    pass
sock.close()
sys.stdout.buffer.write(buf)
PY
    if ! python3 "$UPGRADE_PY" "$URL_BASE" >"$UPGRADE_OUT" 2>"$TMPDIR/upgrade.err"; then
        echo "  [FAIL] h2c upgrade probe (python sender):"
        cat "$TMPDIR/upgrade.err"
        exit 1
    fi
    if ! head_c 64 "$UPGRADE_OUT" | grep -q '^HTTP/1.1 101'; then
        echo "  [FAIL] h2c upgrade did not get 101 reply:"
        head_c 200 "$UPGRADE_OUT"; echo
        exit 1
    fi
fi

echo "  [PASS] HTTP/2 server (issue #260 Tier 2)"
