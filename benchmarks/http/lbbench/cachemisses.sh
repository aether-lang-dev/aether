#!/bin/bash
# Last-level cache misses per proxied request, for aether and for the controls.
#
# This exists because instruction counts sent an optimisation effort in the
# wrong direction. aether executes FEWER instructions per request than nginx
# (18.1k against 23.6k) and is still slower per core, because nearly all of
# nginx's memory references are answered by cache and nearly a hundred of ours
# are not:
#
#     per request, 50 concurrent connections
#     nginx    23,639 instr    521 L1 data misses     1.4 last-level misses
#     aether   18,081 instr    184 L1 data misses    84.8 last-level misses
#
# An L1 miss is answered by L2 or L3 and costs tens of cycles. A last-level
# miss goes to memory and costs hundreds. At roughly 80ns each, eighty-five of
# them is several microseconds per request, which is the whole of the gap.
#
# Two things about how this is measured, both learned by getting them wrong:
#
#   - Concurrency is not optional. Driving one connection shows aether ahead of
#     nginx on every cache metric; the misses only appear when fifty
#     connections take turns and each one's memory has gone cold in between.
#     The default here is deliberately not 1.
#   - Buffer size is not working set. Shrinking the per-connection buffers from
#     16 KiB to 4 KiB changed nothing here, because the pages beyond what a
#     request touches were never read or written in the first place.
#
# Counted under callgrind's cache simulator rather than with perf, so it works
# on a machine with no performance counters, which is the usual case inside a
# VM. The simulated cache is not this machine's, so read the comparison between
# subjects, never the absolute number.
set -uo pipefail

REQUESTS="${REQUESTS:-1200}"
WARM="${WARM:-400}"
CONNECTIONS="${CONNECTIONS:-50}"

say() { printf '%s\n' "$*" >&2; }
die() { say "ERROR: $*"; exit 1; }

. /bench/use_mounted.sh
lbbench_use_mounted cachemisses.sh "$@"

command -v valgrind >/dev/null 2>&1 || die "valgrind is not installed in the image"
command -v python3  >/dev/null 2>&1 || die "python3 is not installed in the image"

nginx -c /bench/backends.conf -p /tmp || die "backends did not start"
for p in 19001 19002; do
    for _ in $(seq 1 40); do curl -sf -o /dev/null "http://127.0.0.1:$p/" && break; sleep 0.25; done
done

say "building ..."
( cd /src && cp -r /src /build ) 2>/dev/null || true
( cd /build && rm -rf build && make -j"$(nproc)" >/tmp/b.log 2>&1 \
  && ./build/ae build benchmarks/http/lb_reuse_lb.ae -o /tmp/ae-lb >>/tmp/b.log 2>&1 ) \
  || { tail -20 /tmp/b.log >&2; die "build failed"; }

# nginx has to run in the foreground and without a master for callgrind to be
# counting the process that actually serves the requests. Everything else is
# /bench/nginx-lb.conf as it stands.
sed -e 's/^daemon on;/daemon off;/' /bench/nginx-lb.conf > /tmp/ng-fg.conf
printf 'master_process off;\n' >> /tmp/ng-fg.conf

# One connection, many requests in turn, so the denominator is exact and every
# connection's memory has gone cold before its next turn.
cat > /tmp/drive.py <<'PYEOF'
import socket, sys
port, total, conc = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
req = b"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"
socks = [socket.create_connection(("127.0.0.1", port), timeout=30) for _ in range(conc)]
for s in socks: s.settimeout(30)
bufs = [b""] * conc
done = 0
try:
    while done < total:
        for i, s in enumerate(socks):
            if done >= total: break
            s.sendall(req)
            while bufs[i].count(b"\r\n\r\n") < 1:
                c = s.recv(65536)
                if not c: raise SystemExit("upstream closed after %d" % done)
                bufs[i] += c
            j = bufs[i].index(b"\r\n\r\n") + 4
            cl = 0
            for line in bufs[i][:j].decode("latin1").split("\r\n"):
                if line.lower().startswith("content-length:"): cl = int(line.split(":")[1])
            while len(bufs[i]) < j + cl:
                c = s.recv(65536)
                if not c: raise SystemExit("upstream closed after %d" % done)
                bufs[i] += c
            bufs[i] = bufs[i][j + cl:]
            done += 1
finally:
    for s in socks:
        try: s.close()
        except Exception: pass
print(done)
PYEOF

stop_lb() { pkill -f '/tmp/ae-lb' >/dev/null 2>&1; nginx -c /tmp/ng-fg.conf -p /tmp -s quit >/dev/null 2>&1; sleep 1; }

# Two runs at different request counts; the difference removes start-up, which
# is otherwise most of what a short run measures.
run_one() {                       # run_one <subject> <requests> <outfile>
    local subject="$1" n="$2" out="$3" pid served
    stop_lb
    rm -f "$out"
    if [ "$subject" = nginx ]; then
        valgrind --tool=callgrind --cache-sim=yes --callgrind-out-file="$out" \
            nginx -c /tmp/ng-fg.conf -p /tmp >/dev/null 2>&1 &
    else
        BACKENDS="http://127.0.0.1:19001;http://127.0.0.1:19002" PORT=18200 \
            valgrind --tool=callgrind --cache-sim=yes --callgrind-out-file="$out" \
            /tmp/ae-lb >/dev/null 2>&1 &
    fi
    pid=$!
    for _ in $(seq 1 150); do
        curl -sf -m 10 -o /dev/null http://127.0.0.1:18200/ 2>/dev/null && break
        sleep 1
    done
    served=$(python3 /tmp/drive.py 18200 "$n" "$CONNECTIONS" 2>&1 | tail -1)
    kill -TERM "$pid" 2>/dev/null
    for _ in $(seq 1 90); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
    case "$served" in
        ''|*[!0-9]*) die "$subject served nothing usable: $served" ;;
    esac
    # Ir Dr Dw I1mr D1mr D1mw ILmr DLmr DLmw
    printf '%s %s\n' "$served" "$(grep -m1 '^summary:' "$out" | sed 's/^summary: //')"
}

report_for() {                    # report_for <subject>
    local subject="$1" a b
    a=$(run_one "$subject" "$WARM" /tmp/cm-a.out) || return
    b=$(run_one "$subject" "$REQUESTS" /tmp/cm-b.out) || return
    printf '%s|%s|%s\n' "$subject" "$a" "$b" | awk -F'|' '{
        split($2, p, " "); split($3, q, " ")
        d = q[1] - p[1]
        if (d <= 0) { printf "  %-8s could not measure\n", $1; next }
        printf "  %-8s %8.0f instr  %7.1f L1d miss  %7.1f last-level miss   per request\n", \
            $1, (q[2]-p[2])/d, ((q[6]-p[6])+(q[7]-p[7]))/d, ((q[9]-p[9])+(q[10]-p[10]))/d
    }'
}

say ""
say "== $CONNECTIONS concurrent connections, $((REQUESTS - WARM)) requests measured =="
report_for aether
report_for nginx
stop_lb
