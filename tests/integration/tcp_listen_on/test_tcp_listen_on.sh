#!/bin/sh
# std.tcp: listen on one address, an ephemeral port read back, a pollable
# server handle, TCP_NODELAY (#2136).
#
# `tcp.listen` bound every interface and refused port 0; `accept` could
# only block, so a server with a stop flag had no way to stop (closing the
# listener from another thread does not wake `accept` on Linux); and
# TCP_NODELAY could not be set at all — `setsockopt` is already declared
# by the runtime's headers with the platform's signature, so an
# `extern setsockopt` of one's own is a conflicting declaration in the C.
# A loopback debugging channel needs all four.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] tcp_listen_on: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT

cat > "$tmp/main.ae" <<'AE'
import std.tcp
import std.worker

// The serving side, on a worker thread: poll the listener with a bounded
// number of 50 ms waits instead of blocking in accept.
serve(job: ptr) -> ptr {
    srv = job
    spins = 0
    while spins < 200 {
        r = tcp.server_poll(srv, 50)
        if r == 1 {
            sock, aerr = tcp.accept(srv)
            if aerr == "" {
                nerr = tcp.set_nodelay(sock, true)
                line, rerr = tcp.read(sock, 64)
                _ = tcp.write(sock, "echo:${line}|nodelay=${nerr == ""}\n")
                _ = tcp.close(sock)
            }
            return null
        }
        spins = spins + 1
    }
    return null
}

main() {
    srv, err = tcp.listen_on("127.0.0.1", 0)
    if err != "" { println("listen: ${err}"); return 1 }
    port = tcp.server_port(srv)
    println("bound ${port > 0 && port < 65536}")
    // Nothing is connecting yet: the poll times out rather than blocking.
    println("idle ${tcp.server_poll(srv, 20)}")
    worker.run(|| { return serve(srv) }, |r: ptr| { })
    sock, cerr = tcp.connect("127.0.0.1", port)
    if cerr != "" { println("connect: ${cerr}"); return 1 }
    _ = tcp.write(sock, "hi")
    got, rerr = tcp.read(sock, 128)
    println(got)
    _ = tcp.close(sock)
    waited = 0
    while worker.pending() > 0 && waited < 5000 { _ = worker.drain(0); sleep(1); waited = waited + 1 }
    bad, berr = tcp.listen_on("not-an-address", 0)
    println("bad ${bad == null} ${berr}")
    println("nullport ${tcp.server_port(null)} nullpoll ${tcp.server_poll(null, 1)}")
    _ = tcp.server_close(srv)
}
AE
want='bound true
idle 0
echo:hi|nodelay=true
bad true listen failed
nullport -1 nullpoll -1'
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/main.ae" 2>&1 | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] tcp_listen_on: output differs"
    printf '%s\n' "$got" | head -8 | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] tcp_listen_on: loopback-only listen, ephemeral port read back, pollable listener, TCP_NODELAY"
exit 0
