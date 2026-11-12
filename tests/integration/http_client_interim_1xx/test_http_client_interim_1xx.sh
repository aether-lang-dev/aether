#!/bin/sh
# Integration test: std.http.client reads past interim 1xx responses.
#
# A server that honours `Expect: 100-continue` answers "100 Continue" and
# then the real response; RFC 9110 15.2 has a client parse any number of
# 1xx responses before the final one. The client returned the 100 as the
# answer (status 100, empty body) and never read the real one. Covered:
# 100 then a pause then 201 (buffered and streamed), the connection reused
# after it, 100 and 103 Early Hints and 200 in one write, and 101, which is
# final and comes back as it is. Found by servirtium-vcr's Azure SDK
# harness (asks/aether-http-client-interim-1xx.md there).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/server_jobs.sh"
AE="$ROOT/build/ae"

RAW_SOCKET_LIBS=""
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) RAW_SOCKET_LIBS="-lws2_32" ;;
esac

if [ ! -x "$AE" ]; then
    echo "  [SKIP] http_client_interim_1xx: ae not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
UP_PID=""
cleanup() { stop_server "$UP_PID"; rm -rf "$TMPDIR"; }
trap cleanup EXIT
fail() { echo "  [FAIL] $1"; exit 1; }

cc -I"$ROOT/tests/lib" "$SCRIPT_DIR/interim_upstream.c" -o "$TMPDIR/upstream" $RAW_SOCKET_LIBS 2>"$TMPDIR/cc.log" \
    || { cat "$TMPDIR/cc.log"; fail "could not compile interim_upstream.c"; }

"$AE" build "$SCRIPT_DIR/interim_client.ae" -o "$TMPDIR/client" > "$TMPDIR/build.log" 2>&1 \
    || { cat "$TMPDIR/build.log"; fail "could not build interim_client.ae"; }

run_case() {                      # run_case <mode> <stream>
    mode="$1"
    "$TMPDIR/upstream" "$mode" > "$TMPDIR/port.txt" 2>/dev/null < /dev/null &
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
    OUT="$(UPSTREAM_PORT="$PORT" MODE="$mode" STREAM="$2" "$TMPDIR/client" 2>&1 < /dev/null)"
    stop_server "$UP_PID"
    UP_PID=""
    : > "$TMPDIR/port.txt"
    if ! echo "$OUT" | grep -q "^PASS"; then
        echo "$OUT"
        fail "mode $mode stream $2"
    fi
}

run_case continue 0
run_case continue 1
run_case multi 0
run_case multi 1
run_case switch 0

echo "  [PASS] http_client_interim_1xx: 1xx interim responses skipped, 101 kept"
