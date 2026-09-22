#!/bin/sh
# `client.set_timeout(req, N)` bounds the WHOLE request, not each recv.
#
# The timeout reached the socket as SO_RCVTIMEO, which limits one recv: a
# peer that sends a byte just inside the limit, over and over, never trips
# it, so a "1 s" request stayed open for as long as the peer cared to
# dribble — the one thing a request timeout exists to prevent. The exchange
# now carries a deadline for the whole response and reports
# "request timed out" when it passes.
#
# The drip server is a few lines of Aether (no python/nc dependency): it
# accepts one connection, writes a Content-Length header promising more
# body than it will ever send, then writes one byte every 200 ms for as
# long as the client is there.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] http_client_request_deadline: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/drip.ae" <<'AE'
import std.tcp
import std.os
import std.string
import std.fs

// A server that answers with a promise it keeps too slowly: 64 body bytes
// declared, one byte every 200 ms. A per-recv timeout never fires on it.
main() {
    srv, err = tcp.listen_on("127.0.0.1", 0)
    if err != "" { println("listen: ${err}"); return 1 }
    // The port goes to a file: a pipe's stdout is buffered, and the
    // driver must see the port before it can connect.
    _ = fs.write(aether_args_get(1), "${tcp.server_port(srv)}")
    sock, aerr = tcp.accept(srv)
    if aerr != "" { return 1 }
    _ = tcp.read(sock, 4096)
    _ = tcp.write(sock, "HTTP/1.1 200 OK\r\nContent-Length: 64\r\n\r\n")
    i = 0
    while i < 64 {
        n, werr = tcp.write(sock, "x")
        if werr != "" { i = 64 } else { sleep(200); i = i + 1 }
    }
    _ = tcp.close(sock)
    _ = tcp.server_close(srv)
}
AE

cat > "$tmp/client.ae" <<'AE'
import std.http.client
import std.os
import std.string

main() {
    port = aether_args_get(1)
    req = client.request("GET", "http://127.0.0.1:${port}/")
    _ = client.set_timeout(req, 1000ms)     // one second for the whole exchange
    t0 = os.now_monotonic_ns()
    resp, serr = client.send_request(req)
    t1 = os.now_monotonic_ns()
    ms = (t1 - t0) / 1000000
    err = serr
    if err == "" && resp != null { err = client.response_error(resp) }
    println("elapsed_under_4s ${ms < 4000}")
    println("error [${err}]")
}
AE

"$AE" build "$tmp/drip.ae" -o "$tmp/drip" >"$tmp/build.log" 2>&1 || {
    echo "  [FAIL] http_client_request_deadline: drip server build failed"
    tail -5 "$tmp/build.log" | sed 's/^/        /'
    exit 1
}
"$AE" build "$tmp/client.ae" -o "$tmp/client" >>"$tmp/build.log" 2>&1 || {
    echo "  [FAIL] http_client_request_deadline: client build failed"
    tail -5 "$tmp/build.log" | sed 's/^/        /'
    exit 1
}

"$tmp/drip" "$tmp/port.txt" > "$tmp/srv.log" 2>&1 &
srv_pid=$!
cleanup() { kill "$srv_pid" 2>/dev/null || true; rm -rf "$tmp" || true; }
trap cleanup EXIT

port=""
i=0
while [ "$i" -lt 100 ]; do
    port="$(cat "$tmp/port.txt" 2>/dev/null)"
    [ -n "$port" ] && break
    sleep 0.1
    i=$((i + 1))
done
if [ -z "$port" ]; then
    echo "  [SKIP] http_client_request_deadline: the drip server did not bind"
    exit 0
fi

out="$("$tmp/client" "$port" 2>&1)"
if ! printf '%s\n' "$out" | grep -q "elapsed_under_4s true"; then
    echo "  [FAIL] http_client_request_deadline: a 1 s timeout did not bound a dribbling response"
    printf '%s\n' "$out" | head -4 | sed 's/^/        /'
    exit 1
fi
if ! printf '%s\n' "$out" | grep -q "timed out"; then
    echo "  [FAIL] http_client_request_deadline: the error does not say it timed out"
    printf '%s\n' "$out" | head -4 | sed 's/^/        /'
    exit 1
fi

echo "  [PASS] http_client_request_deadline: set_timeout bounds the whole response, not each recv"
exit 0
