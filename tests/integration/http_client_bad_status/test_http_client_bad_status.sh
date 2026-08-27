#!/bin/sh
# Integration test: a status line that does not carry a status code.
#
# The code is exactly three digits (RFC 9112 4). Reading it with atoi accepts
# anything starting with a digit and wraps on overflow, so an upstream sending
# `HTTP/1.1 999999999999 Weird` handed the caller status -727379969, and the
# reverse proxy copied that onto the response it sent back to its own client.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] http_client_bad_status on Windows (raw POSIX sockets)"
        exit 0
        ;;
esac

[ -x "$AE" ] || { echo "  [SKIP] http_client_bad_status: ae not built"; exit 0; }

TMPDIR="$(mktemp -d)"
UP_PID=""
cleanup() { [ -n "$UP_PID" ] && kill "$UP_PID" 2>/dev/null; rm -rf "$TMPDIR"; }
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

cc "$SCRIPT_DIR/bad_status_upstream.c" -o "$TMPDIR/upstream" 2>"$TMPDIR/cc.log" \
    || { cat "$TMPDIR/cc.log"; fail "could not compile bad_status_upstream.c"; }
"$TMPDIR/upstream" huge_status > "$TMPDIR/port.txt" 2>/dev/null &
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

OUT="$(UPSTREAM_PORT="$PORT" AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/bad_status.ae" 2>&1)"
if ! echo "$OUT" | grep -q "^PASS"; then
    echo "$OUT"
    fail "an unreadable status line was not reported"
fi

echo "  [PASS] http_client_bad_status: an unreadable status line is reported"
