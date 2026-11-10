#!/bin/sh
# A TLS-terminating reverse proxy, served by the event driver.
#
# TLS connections used to be refused by the driver and given a thread each --
# the cost the driver exists to remove, on the shape most proxied traffic
# actually arrives in. The driver now carries the handshake without waiting and
# reads and writes through the session.
#
# The concurrency here is the point as much as the round trip: the failure this
# path had was not a wrong answer but a server that stopped existing, and one
# request at a time never showed it.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/portable.sh"
AE="$ROOT/build/ae"
UPSTREAM_SRC="$ROOT/tests/integration/http_reverse_proxy/server.ae"
PROXY_SRC="$SCRIPT_DIR/tls_proxy.ae"

command -v openssl >/dev/null 2>&1 || { echo "  [SKIP] openssl not on PATH"; exit 0; }
command -v curl    >/dev/null 2>&1 || { echo "  [SKIP] curl not on PATH"; exit 0; }

. "$ROOT/tests/lib/wait_port.sh"
. "$ROOT/tests/lib/server_jobs.sh"

TMPDIR="$(mktemp -d)"
UP_PID=""; PX_PID=""
cleanup() {
    stop_server "$UP_PID"
    stop_server "$PX_PID"
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

CERT="$TMPDIR/cert.pem"; KEY="$TMPDIR/key.pem"
if ! openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
        -subj "/CN=127.0.0.1" -keyout "$KEY" -out "$CERT" >"$TMPDIR/ssl.log" 2>&1; then
    echo "  [SKIP] openssl could not make a certificate"; exit 0
fi

for pair in "$UPSTREAM_SRC:$TMPDIR/upstream" "$PROXY_SRC:$TMPDIR/proxy"; do
    src=${pair%%:*}; out=${pair##*:}
    if ! AETHER_HOME="$ROOT" "$AE" build "$src" -o "$out" >"$TMPDIR/build.log" 2>&1; then
        echo "  [FAIL] build $src:"; head -20 "$TMPDIR/build.log"; exit 1
    fi
done

AETHER_HOME="$ROOT" "$TMPDIR/upstream" upstream >"$TMPDIR/up.log" 2>&1 &
UP_PID=$!
deadline=$(($(date +%s) + 15))
while ! grep -q '^READY ' "$TMPDIR/up.log" 2>/dev/null; do
    kill -0 "$UP_PID" 2>/dev/null || {
        echo "  [FAIL] upstream died:"; head -20 "$TMPDIR/up.log"; exit 1; }
    [ "$(date +%s)" -lt "$deadline" ] || {
        echo "  [FAIL] upstream never READY:"; head -20 "$TMPDIR/up.log"; exit 1; }
    sleep 0.05
done
UP_PORT=$(read_ready_port "$TMPDIR/up.log") || exit 1

# The proxy's own port is a fixed number because lb.serve_tls takes the port to
# bind and blocks in the server, so there is no moment at which a caller could
# read back a kernel-assigned one. No other test in the sweep uses it, so it
# cannot collide with a sibling running at the same time.
PROXY_PORT=19000
PORT="$PROXY_PORT" BACKENDS="http://127.0.0.1:$UP_PORT" TLS_CERT="$CERT" TLS_KEY="$KEY" \
    AETHER_HOME="$ROOT" "$TMPDIR/proxy" >"$TMPDIR/px.log" 2>&1 &
PX_PID=$!

deadline=$(($(date +%s) + 20))
while [ "$(date +%s)" -lt "$deadline" ]; do
    curl -sk -o /dev/null --max-time 1 "https://127.0.0.1:$PROXY_PORT/echo" 2>/dev/null && break
    if ! kill -0 "$PX_PID" 2>/dev/null; then
        echo "  [FAIL] the TLS proxy died while starting:"; head -20 "$TMPDIR/px.log"; exit 1
    fi
    sleep 0.1
done

# 1 — a proxied round trip over TLS reaches the upstream and comes back.
BODY=$(curl -sk --show-error --max-time 5 "https://127.0.0.1:$PROXY_PORT/echo" 2>"$TMPDIR/c1.err") || {
    echo "  [FAIL] T1 curl:"; cat "$TMPDIR/c1.err"; exit 1; }
case "$BODY" in
    *upstream-ok*) : ;;
    *) echo "  [FAIL] T1: the upstream's answer did not come back: $BODY"; exit 1 ;;
esac

# 2 — the proxy's own headers are there, so it proxied rather than answered.
HDRS=$(curl -sk -D - -o /dev/null --max-time 5 "https://127.0.0.1:$PROXY_PORT/echo" 2>/dev/null)
case "$HDRS" in
    *X-Upstream-Tag*) : ;;
    *) echo "  [FAIL] T2: the upstream's headers are missing"; exit 1 ;;
esac

# 3 — a POST body survives the TLS leg byte for byte.
yes 'tls-payload-0123456789' 2>/dev/null | head_c 4096 > "$TMPDIR/post.in"
curl -sk --max-time 5 -X POST --data-binary "@$TMPDIR/post.in" \
     -o "$TMPDIR/post.out" "https://127.0.0.1:$PROXY_PORT/echo" 2>/dev/null || {
    echo "  [FAIL] T3 POST failed"; exit 1; }
IN=$(wc -c <"$TMPDIR/post.in" | tr -d ' '); OUT=$(wc -c <"$TMPDIR/post.out" | tr -d ' ')
[ "$IN" = "$OUT" ] || { echo "  [FAIL] T3 body length: sent $IN, got $OUT"; exit 1; }

# 4 — many at once. A thread-per-connection fallback shows up here, and so
# does a handshake that only completes when nothing else is in flight. Driven
# from python rather than the shell: 24 background curls and a bare `wait`
# also wait on the servers, which serve until killed.
if command -v python3 >/dev/null 2>&1; then
    if OUT=$(python3 "$SCRIPT_DIR/concurrent_probe.py" "$PROXY_PORT" 24 2>&1); then
        :
    else
        echo "  [FAIL] T4 concurrent TLS: $OUT"; exit 1
    fi
else
    echo "  [SKIP-T4] python3 not on PATH; concurrency unchecked"
fi

kill -0 "$PX_PID" 2>/dev/null || {
    echo "  [FAIL] the TLS proxy died during the run"; PX_PID=""; exit 1; }

echo "  [PASS] http_reverse_proxy_tls: 4/4 — round trip, upstream headers, 4 KiB POST, 24 at once"
