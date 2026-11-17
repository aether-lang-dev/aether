#!/bin/sh
# #260 Tier 1 / Phase D1: 4 middleware end-to-end.
#
# Verifies each middleware in turn against the running server:
#   - vhost  : Host: foo.example -> 404 "Unknown host"
#   - auth   : missing Authorization -> 401 with WWW-Authenticate
#              wrong credentials -> 401
#              correct credentials (alice:secret) -> 200
#   - cors   : OPTIONS preflight -> 204 with Access-Control headers
#              GET response carries the configured headers
#   - rate   : 11th request inside the 5s window -> 429 with
#              Retry-After
#
# Skips when curl(1) is missing.

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
        echo "  [SKIP-WIN] http_middleware_d1 — HTTP server/proxy/middleware code is platform-independent; covered by POSIX matrix"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"

if ! command -v curl >/dev/null 2>&1; then
    echo "  [SKIP] curl not on PATH"
    exit 0
fi

TMPDIR="$(mktemp -d)"
cleanup() {
    stop_server "${SRV_PID:-}"
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/server.ae" >"$TMPDIR/srv.log" 2>&1 &

# The server binds port 0 and prints the kernel's choice on its READY line.
for _ in $(seq 1 300); do
    grep -q "^READY " "$TMPDIR/srv.log" 2>/dev/null && break
    sleep 0.05
done
PORT=$(read_ready_port "$TMPDIR/srv.log") || exit 1
SRV_PID=$!

URL="http://127.0.0.1:$PORT/"

# Wait for the server to actually accept connections, not just print
# READY. server.ae prints READY before the SrvActor receives StartSrv
# and calls http_server_start_raw — on slow runners (Windows GHA)
# that gap can be ~hundreds of ms. Probe the port until any HTTP
# response comes back (auth-middleware 401 still means we're in).
deadline=$(($(date +%s) + 15))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if ! kill -0 "$SRV_PID" 2>/dev/null; then
        echo "  [FAIL] server died:"
        head -20 "$TMPDIR/srv.log"
        exit 1
    fi
    if curl -s -o /dev/null --max-time 1 "$URL" 2>/dev/null; then
        break
    fi
    sleep 0.1
done
if [ "$(date +%s)" -ge "$deadline" ]; then
    echo "  [FAIL] server never accepted connections within 15s"
    head -20 "$TMPDIR/srv.log"
    exit 1
fi

# --- vhost: unknown host -> 404 ---
status=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
    -H "Host: foo.example" "$URL")
if [ "$status" != "404" ]; then
    echo "  [FAIL] vhost: expected 404 for foo.example; got $status"
    exit 1
fi

# --- auth: missing creds -> 401 + WWW-Authenticate ---
HEADERS="$TMPDIR/h1"
curl -s -D "$HEADERS" -o /dev/null --max-time 5 -H "Host: localhost" "$URL"
if ! grep -qi "^HTTP/1.1 401" "$HEADERS"; then
    echo "  [FAIL] auth: expected 401 without creds"
    cat "$HEADERS"
    exit 1
fi
if ! grep -qi "WWW-Authenticate: Basic realm=" "$HEADERS"; then
    echo "  [FAIL] auth: missing WWW-Authenticate header"
    cat "$HEADERS"
    exit 1
fi

# --- auth: wrong creds -> 401 ---
status=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
    -u "alice:wrong" -H "Host: localhost" "$URL")
if [ "$status" != "401" ]; then
    echo "  [FAIL] auth: wrong creds expected 401; got $status"
    exit 1
fi

# --- auth: `=` inside the credentials -> 400 ---
# "YWxpY2U6c2VjcmV0" is alice:secret. With an `=` in the middle the old
# private decoder in aether_middleware.c skipped it and let alice in; the
# shared std.cryptography decoder takes `=` only as trailing padding.
status=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
    -H "Authorization: Basic YWxp=Y2U6c2VjcmV0" -H "Host: localhost" "$URL")
if [ "$status" != "400" ]; then
    echo "  [FAIL] auth: '=' inside the base64 expected 400; got $status"
    exit 1
fi

# --- auth: correct creds -> 200 ---
HEADERS="$TMPDIR/h2"
BODY="$TMPDIR/b2"
curl -s -D "$HEADERS" -o "$BODY" --max-time 5 \
    -u "alice:secret" -H "Host: localhost" "$URL"
if ! grep -qi "^HTTP/1.1 200" "$HEADERS"; then
    echo "  [FAIL] auth: alice:secret expected 200"
    cat "$HEADERS"
    exit 1
fi
if [ "$(cat "$BODY")" != "ok-mw" ]; then
    echo "  [FAIL] auth: body wrong; got '$(cat "$BODY")'"
    exit 1
fi

# --- cors: GET response carries Access-Control-Allow-Origin ---
if ! grep -qi "Access-Control-Allow-Origin: \\*" "$HEADERS"; then
    echo "  [FAIL] cors: missing Access-Control-Allow-Origin on GET response"
    cat "$HEADERS"
    exit 1
fi

# --- cors: OPTIONS preflight short-circuits with 204 + Max-Age ---
HEADERS="$TMPDIR/h3"
curl -s -D "$HEADERS" -o /dev/null --max-time 5 -X OPTIONS \
    -H "Host: localhost" -u "alice:secret" "$URL"
if ! grep -qi "^HTTP/1.1 204" "$HEADERS"; then
    echo "  [FAIL] cors: OPTIONS preflight expected 204"
    cat "$HEADERS"
    exit 1
fi
if ! grep -qi "Access-Control-Max-Age: 600" "$HEADERS"; then
    echo "  [FAIL] cors: missing Access-Control-Max-Age: 600"
    cat "$HEADERS"
    exit 1
fi

# --- rate_limit: budget is 10 per 5s; the auth-success + OPTIONS
# preflight runs above already consumed ~2 tokens (auth-failed
# requests are short-circuited BEFORE rate_limit, so they don't
# count). Burst 15 more — past the 10-token budget — and expect
# at least one 429 with a Retry-After header.
# Capture headers on every request so when we hit the first 429 we
# can verify Retry-After is on THAT response — firing a second curl
# to fetch the headers would race the bucket refill window, which on
# slower runners is enough for the next request to pass.
got_429=0
for n in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    H="$TMPDIR/hr.$n"
    status=$(curl -s -D "$H" -o /dev/null -w '%{http_code}' --max-time 5 \
        -u "alice:secret" -H "Host: localhost" "$URL")
    if [ "$status" = "429" ]; then
        got_429=1
        if ! grep -qi "Retry-After:" "$H"; then
            echo "  [FAIL] rate_limit: 429 without Retry-After header"
            cat "$H"
            exit 1
        fi
        break
    fi
done
if [ "$got_429" -ne 1 ]; then
    echo "  [FAIL] rate_limit: budget never exhausted across 15 extra requests"
    exit 1
fi

echo "  [PASS] http_middleware_d1: vhost / basic_auth / cors / rate_limit all enforced"
