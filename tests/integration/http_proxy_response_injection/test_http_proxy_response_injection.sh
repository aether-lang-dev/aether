#!/bin/sh
# The reverse proxy must not forward a header whose value carries a bare CR or
# LF. Such a value does not end a line on the wire, so it reaches the code that
# writes the client's head intact; written out verbatim it would end that head
# early and let the bytes behind it be read as headers the upstream never sent
# (CWE-113, response splitting).
#
# The upstream here is hostile and speaks raw bytes, which is the only way to
# send a header value an HTTP server would never produce.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v python3 >/dev/null 2>&1; then
    echo "  [SKIP] http_proxy_response_injection: python3 not on PATH"
    exit 0
fi

TMPDIR="$(mktemp -d)"
PX_PID=""
cleanup() {
    if [ -n "$PX_PID" ]; then
        kill "$PX_PID" 2>/dev/null || true
        wait "$PX_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

if ! AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/server.ae" \
        -o "$TMPDIR/server" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] build:"; head -30 "$TMPDIR/build.log"; exit 1
fi

# Both paths, because they defend against this separately and a request is
# served by one or the other: the pass-through checks the value and drops that
# header, while the copying path stops parsing the block at the malformed line.
# Testing only whichever happens to be active would leave the other unguarded.
run_one() {
    label="$1"; direct="$2"
    # Only the proxy: the probe binds 19001 itself and answers as the upstream.
    AETHER_PROXY_DIRECT="$direct" AETHER_HOME="$ROOT" "$TMPDIR/server" proxy \
        >"$TMPDIR/px.$label.log" 2>&1 &
    PX_PID=$!

    deadline=$(($(date +%s) + 15))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if ! kill -0 "$PX_PID" 2>/dev/null; then
            echo "  [FAIL] proxy died ($label):"; head -30 "$TMPDIR/px.$label.log"; exit 1
        fi
        # 502 is expected: nothing answers upstream yet. Any reply at all means
        # the proxy is accepting.
        if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:19000/echo" 2>/dev/null; then
            break
        fi
        sleep 0.1
    done

    if ! OUT=$(python3 "$SCRIPT_DIR/injection_probe.py" 2>&1); then
        echo "  [FAIL] http_proxy_response_injection ($label): $OUT"; exit 1
    fi

    kill "$PX_PID" 2>/dev/null || true
    wait "$PX_PID" 2>/dev/null || true
    PX_PID=""
    sleep 0.3
}

run_one pass-through 1
run_one copying 0

echo "  [PASS] http_proxy_response_injection: 2/2 paths - bare CR and LF in upstream header values do not reach the client"
