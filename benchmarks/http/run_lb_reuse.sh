#!/bin/bash
# What connection reuse is worth to std.http.server.lb (#1653).
#
# Runs the same load through the same load balancer twice: once with the
# client's idle connection pool on (the default), once with it off, so the
# upstream half is measured on its own. The inbound half is the server's
# keep-alive, which is on whenever nothing is queued for a worker.
#
# ab ships with macOS and most distributions; wrk or oha would do as well, and
# absolute numbers are worth less than the ratio between the two columns on
# one box.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT_DIR"

command -v ab >/dev/null 2>&1 || { echo "ERROR: ab (ApacheBench) is not installed." >&2; exit 1; }
[ -x build/ae ] || { echo "ERROR: build/ae is missing (run make)." >&2; exit 1; }

REQUESTS="${REQUESTS:-3000}"
CONCURRENCIES="${CONCURRENCIES:-4 8 20 50}"
BACKEND_PORTS="${BACKEND_PORTS:-18201 18202}"
LB_PORT="${LB_PORT:-18200}"

TMP="$(mktemp -d)"
PIDS=""
cleanup() { for p in $PIDS; do kill "$p" 2>/dev/null || true; done; rm -rf "$TMP"; }
trap cleanup EXIT

./build/ae build benchmarks/http/lb_reuse_backend.ae -o "$TMP/backend" >/dev/null
./build/ae build benchmarks/http/lb_reuse_lb.ae      -o "$TMP/lb"      >/dev/null

BACKENDS=""
for p in $BACKEND_PORTS; do
    PORT="$p" "$TMP/backend" >/dev/null 2>&1 &
    PIDS="$PIDS $!"
    [ -n "$BACKENDS" ] && BACKENDS="$BACKENDS;"
    BACKENDS="${BACKENDS}http://127.0.0.1:$p"
done
sleep 1

rps() {   # rps <url> <concurrency>
    ab -k -c "$2" -n "$REQUESTS" "$1" 2>/dev/null |
        awk '/Requests per second/ {printf "%.0f", $4}'
}

start_lb() {   # start_lb [NO_UPSTREAM_REUSE]
    PORT="$LB_PORT" BACKENDS="$BACKENDS" NO_UPSTREAM_REUSE="$1" "$TMP/lb" >/dev/null 2>&1 &
    LB_PID=$!
    PIDS="$PIDS $LB_PID"
    sleep 1
}

echo "=== std.http.server.lb: upstream connection reuse ==="
echo "date:     $(date)"
echo "requests: $REQUESTS per measurement, two backends"
echo
printf '%-12s %14s %14s %8s\n' "concurrency" "reuse off" "reuse on" "gain"

for c in $CONCURRENCIES; do
    start_lb 1
    off="$(rps "http://127.0.0.1:$LB_PORT/" "$c")"
    kill "$LB_PID" 2>/dev/null || true
    sleep 1

    start_lb ""
    on="$(rps "http://127.0.0.1:$LB_PORT/" "$c")"
    kill "$LB_PID" 2>/dev/null || true
    sleep 1

    if [ -n "$off" ] && [ "$off" -gt 0 ] 2>/dev/null; then
        gain="$(awk -v a="$on" -v b="$off" 'BEGIN {printf "%.2fx", a / b}')"
    else
        gain="n/a"
    fi
    printf '%-12s %14s %14s %8s\n' "$c" "$off" "$on" "$gain"
done
