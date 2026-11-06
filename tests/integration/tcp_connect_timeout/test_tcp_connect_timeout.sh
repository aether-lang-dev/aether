#!/bin/sh
# std.tcp.connect_timeout: a dial held to a budget.
#
# tcp.connect had no timeout, so a connect to an address that drops the SYN
# (a firewalled port, a host that is down, a container's port not yet
# forwarded) blocked for the kernel's own retry schedule: 75 s on macOS,
# about two minutes on Linux. A readiness wait polling a database port
# (testcontainers-ae's MySQL and Postgres waits) stalled for that long.
#
#   1. 192.0.2.1 (TEST-NET-1, RFC 5737: never routed) with 300 ms gives
#      "connect timeout" in well under a second. A host with no route at all
#      fails it at once with "connection failed" instead; that is reported,
#      and still has to come back inside the budget.
#   2. A connect_timeout to a listener on loopback connects and talks.
#   3. A port nothing listens on is refused, not timed out. On Windows the
#      refusal still comes back inside the budget but not fast: Winsock
#      retransmits the SYN on its own schedule after the RST before it
#      reports a non-blocking connect failed (seen on the CI runners: the
#      2 s budget ran out first), so there only "inside the budget" is held.
#   4. A timeout that is not positive is refused before any dial.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=tcp_connect_timeout

if [ ! -x "$AE" ]; then
    echo "  [SKIP] $NAME: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT

cat > "$tmp/main.ae" <<'AE'
import std.tcp
import std.time
import std.worker

serve(job: ptr) -> ptr {
    srv = job
    spins = 0
    while spins < 200 {
        if tcp.server_poll(srv, 50) == 1 {
            sock, aerr = tcp.accept(srv)
            if aerr == "" {
                line, rerr = tcp.read(sock, 64)
                _ = tcp.write(sock, "echo:${line}")
                _ = tcp.close(sock)
            }
            return null
        }
        spins = spins + 1
    }
    return null
}

main() {
    t0 = time.now_ms()
    s, err = tcp.connect_timeout("192.0.2.1", 9, 300)
    el = time.now_ms() - t0
    if err == "connection failed" {
        println("blackhole: no route on this host (${err}) in ${el} ms")
        println("blackhole bounded ${el < 1000}")
    } else {
        println("blackhole ${s == null} ${err}")
        println("blackhole bounded ${el >= 250 && el < 1000}")
    }

    srv, lerr = tcp.listen_on("127.0.0.1", 0)
    if lerr != "" { println("listen: ${lerr}"); return 1 }
    port = tcp.server_port(srv)
    worker.run(|| { return serve(srv) }, |r: ptr| { })
    sock, cerr = tcp.connect_timeout("127.0.0.1", port, 2000)
    if cerr != "" { println("connect: ${cerr}"); return 1 }
    _ = tcp.write(sock, "hi")
    got, rerr = tcp.read(sock, 64)
    println("local ${got}")
    _ = tcp.close(sock)
    waited = 0
    while worker.pending() > 0 && waited < 5000 { _ = worker.drain(0); sleep(1); waited = waited + 1 }
    _ = tcp.server_close(srv)

    // The listener is gone: its port is refused, at once.
    t1 = time.now_ms()
    r, rferr = tcp.connect_timeout("127.0.0.1", port, 2000)
    el1 = time.now_ms() - t1
    println("refused ${r == null} ${rferr} fast ${el1 < 1000} bounded ${el1 < 2600} in ${el1} ms")

    z, zerr = tcp.connect_timeout("127.0.0.1", port, 0)
    println("zero ${z == null} ${zerr}")
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/main.ae" 2>&1 | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
printf '%s\n' "$got" | grep '^refused' | sed 's/^/    note: /'
refused_want='refused true connection failed fast true bounded true'
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        # Either error, however long Winsock took, as long as it was inside the budget.
        got="$(printf '%s\n' "$got" | sed -E 's/^refused true (connection failed|connect timeout) fast (true|false) bounded/refused true - fast - bounded/')"
        refused_want='refused true - fast - bounded true' ;;
esac
got="$(printf '%s\n' "$got" | sed -E 's/^(refused .*) in [0-9]+ ms$/\1/')"
case "$got" in
    "blackhole: no route"*)
        printf '%s\n' "$got" | head -1 | sed 's/^/    note: /'
        got="$(printf '%s\n' "$got" | sed 1d | sed 's/^blackhole bounded/blackhole true connect timeout\
blackhole bounded/')" ;;
esac
want="blackhole true connect timeout
blackhole bounded true
local echo:hi
$refused_want
zero true timeout_ms must be positive"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] $NAME: output differs"
    printf '%s\n' "$got" | head -12 | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] $NAME: a blackholed address times out inside the budget; loopback connects; refused is refused"
exit 0
