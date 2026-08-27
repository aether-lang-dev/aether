#!/bin/sh
# Integration test: a response has to say where its body ends, once.
#
# Two Content-Length headers that disagree name no single end, and the bytes
# the client does not account for sit in a connection it may hand to the next
# request. One length with more bytes behind it is the same problem from the
# other side: only the declared body belongs to that response.
#
# This matters more on a client that pools connections than on one that does
# not, because a mis-framed response leaves the surplus where the next
# response's head is expected.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP] http_client_response_framing on Windows (raw POSIX sockets)"
        exit 0
        ;;
esac

if [ ! -x "$AE" ]; then
    echo "  [SKIP] http_client_response_framing: ae not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
UP_PID=""
cleanup() { [ -n "$UP_PID" ] && kill "$UP_PID" 2>/dev/null; rm -rf "$TMPDIR"; }
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

cc "$SCRIPT_DIR/framing_upstream.c" -o "$TMPDIR/upstream" 2>"$TMPDIR/cc.log" \
    || { cat "$TMPDIR/cc.log"; fail "could not compile framing_upstream.c"; }

run_case() {                      # run_case <mode>
    mode="$1"
    "$TMPDIR/upstream" "$( [ "$mode" = extra ] && echo extra || echo dup )" > "$TMPDIR/port.txt" 2>/dev/null &
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
           "$AE" run "$SCRIPT_DIR/response_framing.ae" 2>&1)"
    kill "$UP_PID" 2>/dev/null
    UP_PID=""
    : > "$TMPDIR/port.txt"
    if ! echo "$OUT" | grep -q "^PASS"; then
        echo "$OUT"
        fail "mode $mode"
    fi
}

run_case ambiguous
run_case extra

echo "  [PASS] http_client_response_framing: two lengths refused, surplus bytes not delivered"
