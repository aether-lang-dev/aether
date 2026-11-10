#!/bin/sh
# #260 Tier 2: HTTP/2 + middleware integration end-to-end.
#
# Verifies that the existing middleware chain (cors) and response-
# transformer chain (gzip) fire identically on h2 streams as they
# do on HTTP/1.1 requests. The handler emits a large text body;
# we expect:
#   - Access-Control-Allow-Origin: * on the response (cors)
#   - Content-Encoding: gzip on the response (gzip transformer)
#   - the body decompresses to the expected text
#   - http_version=2 (curl reports HTTP/2)
#
# Skips cleanly when curl, libnghttp2, or gzip is missing.

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
        echo "  [SKIP-WIN] http_server_h2_middleware — HTTP server/proxy/middleware code is platform-independent; covered by POSIX matrix"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/portable.sh"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"

if ! command -v curl >/dev/null 2>&1; then
    echo "  [SKIP] curl not on PATH"; exit 0
fi
if ! curl --version 2>&1 | grep -q nghttp2; then
    echo "  [SKIP] curl built without HTTP/2"; exit 0
fi
if ! command -v gzip >/dev/null 2>&1; then
    echo "  [SKIP] gzip not on PATH"; exit 0
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
PORT=$(read_ready_port "$TMPDIR/srv.log") || exit 1
wait_port "$PORT" || exit 1

URL="http://127.0.0.1:$PORT/text"
HEADERS="$TMPDIR/headers"
BODY_GZ="$TMPDIR/body.gz"
HTTPVER=$(curl --silent --show-error --max-time 5 \
              --http2-prior-knowledge \
              --header 'Origin: https://example.test' \
              --header 'Accept-Encoding: gzip' \
              --dump-header "$HEADERS" \
              -o "$BODY_GZ" \
              -w '%{http_version}' \
              "$URL" 2>"$TMPDIR/curl.err") || {
    echo "  [FAIL] curl failed:"; cat "$TMPDIR/curl.err"; exit 1
}

if [ "$HTTPVER" != "2" ]; then
    echo "  [FAIL] expected http_version=2, got '$HTTPVER'"; exit 1
fi

# cors header must be present.
if ! grep -qi '^access-control-allow-origin:' "$HEADERS"; then
    echo "  [FAIL] cors header missing on h2 response:"
    cat "$HEADERS"; exit 1
fi

# gzip transformer must have fired.
if ! grep -qi '^content-encoding: *gzip' "$HEADERS"; then
    echo "  [FAIL] gzip Content-Encoding missing on h2 response:"
    cat "$HEADERS"; exit 1
fi

# Body must round-trip through gunzip and start with the expected
# text. We don't compare the entire body byte-for-byte since the
# server's response_set_body wrapping isn't NUL-bound and the
# string-interpolation builder may produce different lengths under
# different builds; the prefix check is enough to prove the
# pipeline worked end-to-end.
if ! gunzip -c "$BODY_GZ" | head_c 36 | grep -q '^abcdefghijklmnopqrstuvwxyz0123456789$'; then
    echo "  [FAIL] decompressed body prefix mismatch:"
    gunzip -c "$BODY_GZ" 2>&1 | head_c 100; echo
    exit 1
fi

echo "  [PASS] HTTP/2 + middleware integration (issue #260 Tier 2)"
