#!/bin/sh
# Integration test: a response cut short of its declared length is an error.
#
# A server that sends `Content-Length: 36` and then closes after 10 bytes has
# not sent the response it promised. Returning the 10 bytes as a successful
# 200 makes a truncated payload indistinguishable from a complete one: a proxy
# forwards a short body as whole, and a caller parses whatever arrived.
#
# The second half is the guard on the fix: a response that declares no framing
# at all is ended BY the close, so the same 10 bytes are complete there and
# must still be accepted.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] http_client_truncated on Windows (raw POSIX sockets)"
        exit 0
        ;;
esac

if [ ! -x "$AE" ]; then
    echo "  [SKIP] http_client_truncated: ae not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
UP_PID=""
cleanup() { [ -n "$UP_PID" ] && kill "$UP_PID" 2>/dev/null; rm -rf "$TMPDIR"; }
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

cc "$SCRIPT_DIR/truncated_upstream.c" -o "$TMPDIR/upstream" 2>"$TMPDIR/cc.log" \
    || { cat "$TMPDIR/cc.log"; fail "could not compile truncated_upstream.c"; }

run_case() {                      # run_case <mode>
    mode="$1"
    "$TMPDIR/upstream" "$mode" > "$TMPDIR/port.txt" 2>/dev/null &
    UP_PID=$!
    PORT=""
    i=0
    while [ "$i" -lt 100 ]; do
        PORT="$(head -n1 "$TMPDIR/port.txt" 2>/dev/null)"
        [ -n "$PORT" ] && break
        sleep 0.05
        i=$((i + 1))
    done
    [ -n "$PORT" ] || fail "upstream did not report a port for mode $mode"

    OUT="$(UPSTREAM_PORT="$PORT" MODE="$mode" AETHER_HOME="$ROOT" \
           "$AE" run "$SCRIPT_DIR/truncated.ae" 2>&1)"
    kill "$UP_PID" 2>/dev/null
    UP_PID=""
    : > "$TMPDIR/port.txt"
    if ! echo "$OUT" | grep -q "^PASS"; then
        echo "$OUT"
        fail "mode $mode"
    fi
}

run_case short
run_case close

echo "  [PASS] http_client_truncated: a short body is an error, a close-framed body is not"
