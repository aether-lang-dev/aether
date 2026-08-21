#!/bin/sh
# #1663: a server holds more concurrent keep-alive connections than it
# has workers.
#
# The worker pool is cores*2 (min 8, max 64) and a worker used to own a
# connection for its whole life, so the server could hold only that many
# connections open at once — the rest stalled until one was released.
# With parking, an idle keep-alive connection is handed to a poller and
# costs a file descriptor rather than a thread.
#
# The test opens 3x the worker count of simultaneous connections and sends
# several requests down each. Every request must succeed AND the
# connections must actually be reused: without parking the server falls
# back to close-per-response once workers are scarce (the #1653 capacity
# rule), so curl silently reconnects and every request still returns 200.
# Success alone therefore proves nothing — the reuse count is the
# assertion that distinguishes parked from unparked.

# Skip on Windows — parking is POSIX-only (AETHER_HTTP_PARK), and the
# server code under test is platform-independent userland C already
# covered by the Linux and macOS matrix entries. Each curl invocation
# under MSYS2 pays Cygwin fork-emulation overhead, so this would add
# minutes without adding coverage.
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] http_keepalive_parking — parking is POSIX-only; covered by POSIX matrix"
        exit 0
        ;;
esac

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
PORT=18104

if ! command -v curl >/dev/null 2>&1; then
    echo "  [SKIP] curl not on PATH"
    exit 0
fi

TMPDIR="$(mktemp -d)"
cleanup() {
    if [ -n "${SRV_PID:-}" ]; then
        kill "$SRV_PID" 2>/dev/null || true
        wait "$SRV_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT INT TERM

"$AE" build "$SCRIPT_DIR/server.ae" -o "$TMPDIR/server" >/dev/null 2>&1 || {
    echo "  [FAIL] could not build server.ae"
    exit 1
}

"$TMPDIR/server" > "$TMPDIR/srv.log" 2>&1 &
SRV_PID=$!

# Wait for the port to answer rather than for a READY line plus a sleep:
# a fixed sleep is the usual source of flakes on a loaded box. /dev/tcp is
# a bash builtin and this runs under sh, so probe with curl instead.
i=0
while [ "$i" -lt 100 ]; do
    # Require the route's own body, not merely a connection: the listener
    # is up a moment before server_get registers the route, and probing
    # with a bare connect gets a 404 from that window.
    if curl -s --max-time 2 "http://127.0.0.1:$PORT/" 2>/dev/null | grep -q 'park-ok'; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done
if [ "$i" -ge 100 ]; then
    echo "  [FAIL] server never accepted on $PORT"
    sed -n '1,10p' "$TMPDIR/srv.log"
    exit 1
fi

# Worker count is cores*2 clamped to [8,64]; mirror that so the load is
# meaningfully above it on any box.
CORES=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
WORKERS=$((CORES * 2))
[ "$WORKERS" -lt 8 ] && WORKERS=8
[ "$WORKERS" -gt 64 ] && WORKERS=64
CONNS=$((WORKERS * 3))
REQS_PER_CONN=4

# Each client keeps ONE connection open for REQS_PER_CONN requests
# (curl's multi-URL form reuses the connection), and they all run at
# once. Every request must return 200 with the expected body.
URLS=""
n=0
while [ "$n" -lt "$REQS_PER_CONN" ]; do
    URLS="$URLS http://127.0.0.1:$PORT/"
    n=$((n + 1))
done

# -o applies per-URL, so give each request its own body file and keep the
# status codes on stdout; writing both to one stream interleaves them.
c=0
while [ "$c" -lt "$CONNS" ]; do
    outs=""
    n=0
    while [ "$n" -lt "$REQS_PER_CONN" ]; do
        outs="$outs -o $TMPDIR/body.$c.$n"
        n=$((n + 1))
    done
    # num_connects counts TCP connections actually established; with the
    # connection reused across all REQS_PER_CONN requests it is 1.
    # shellcheck disable=SC2086
    curl -s --max-time 30 $outs -w '%{http_code} %{num_connects}\n' $URLS \
        > "$TMPDIR/code.$c" 2>/dev/null &
    c=$((c + 1))
done
wait

ok=0
bad=0
TOTAL_CONNECTS=0
c=0
while [ "$c" -lt "$CONNS" ]; do
    codes=$(awk '{print $1}' "$TMPDIR/code.$c" 2>/dev/null || echo "")
    connects=$(awk '{s+=$2} END{print s+0}' "$TMPDIR/code.$c" 2>/dev/null || echo 0)
    TOTAL_CONNECTS=$((TOTAL_CONNECTS + connects))
    got=$(printf '%s\n' "$codes" | grep -c '^200$' || true)
    # The body carries no trailing newline, so concatenated files run
    # together on one line: count occurrences, not matching lines.
    body_ok=$(cat "$TMPDIR"/body."$c".* 2>/dev/null | grep -o 'park-ok' | wc -l)
    if [ "$got" -eq "$REQS_PER_CONN" ] && [ "$body_ok" -eq "$REQS_PER_CONN" ]; then
        ok=$((ok + 1))
    else
        bad=$((bad + 1))
        [ "$bad" -le 3 ] && echo "  conn $c: $got/$REQS_PER_CONN x 200 (codes: $(printf '%s' "$codes" | tr '\n' ' '))"
    fi
    c=$((c + 1))
done

TOTAL=$((CONNS * REQS_PER_CONN))
if [ "$bad" -ne 0 ]; then
    echo "  [FAIL] http_keepalive_parking: $bad/$CONNS connections incomplete"
    echo "         ($WORKERS workers, $CONNS concurrent connections, $TOTAL requests)"
    exit 1
fi

# One TCP connection per client is the parked ideal ($CONNS total).
# Without parking the server closes between requests whenever workers are
# scarce, so the count climbs: measured 102 connects unparked against 72
# parked, for 72 clients / 288 requests on a 12-core box. Allow a fifth of
# the reconnects the unparked path needs — enough slack for an occasional
# genuine close under load, tight enough that losing parking fails here.
LIMIT=$(( CONNS + (TOTAL - CONNS) / 5 ))
if [ "$TOTAL_CONNECTS" -gt "$LIMIT" ]; then
    echo "  [FAIL] http_keepalive_parking: connections were not reused"
    echo "         $TOTAL_CONNECTS TCP connects for $TOTAL requests over $CONNS clients"
    echo "         (expected <= $LIMIT; $CONNS would be perfect reuse)"
    exit 1
fi

echo "  [PASS] http_keepalive_parking: $CONNS concurrent keep-alive connections ($WORKERS workers), $TOTAL requests all 200, $TOTAL_CONNECTS TCP connects"
