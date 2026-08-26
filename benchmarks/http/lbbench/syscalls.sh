#!/bin/bash
# Syscalls per proxied request, counted exactly.
#
# Throughput on a shared or virtualised box can be too noisy to resolve a
# change worth a few percent, and this harness has produced runs where the
# controls moved 198%. Syscall counts do not vary with load: they are what the
# code asks the kernel to do. When #1719 found aether making ~10 syscalls per
# request against nginx's ~5, that was this measurement, and it is the one to
# check a syscall change against.
#
# AB_REF counts a second build from that commit, so the difference is exact.
set -uo pipefail

REQUESTS="${REQUESTS:-2000}"
CONNECTIONS="${CONNECTIONS:-8}"

say() { printf '%s\n' "$*" >&2; }
die() { say "ERROR: $*"; exit 1; }

command -v strace >/dev/null 2>&1 || die "strace is not installed in the image"

nginx -c /bench/backends.conf -p /tmp || die "backends did not start"
for p in 19001 19002; do
    for _ in $(seq 1 40); do curl -sf -o /dev/null "http://127.0.0.1:$p/" && break; sleep 0.25; done
done

build_lb() {                      # build_lb <srcdir> <out>
    ( cd "$1" && rm -rf build && make -j"$(nproc)" >/tmp/b.log 2>&1 \
      && ./build/ae build benchmarks/http/lb_reuse_lb.ae -o "$2" >>/tmp/b.log 2>&1 ) \
      || { tail -20 /tmp/b.log >&2; die "build failed for $1"; }
}

say "building ..."
cp -r /src /build && build_lb /build /tmp/ae-lb
AB_REF="${AB_REF:-}"
if [ -n "$AB_REF" ]; then
    git clone -q /src /baseline || die "cannot clone /src"
    ( cd /baseline && git checkout -q "$AB_REF" ) || die "no such ref: $AB_REF"
    build_lb /baseline /tmp/ae-lb-base
fi

count_for() {                     # count_for <binary> <label>
    local bin="$1" label="$2" out
    pkill -f '/tmp/ae-lb' >/dev/null 2>&1; sleep 1
    BACKENDS="http://127.0.0.1:19001;http://127.0.0.1:19002" PORT=18200 \
        strace -f -c -o /tmp/strace-"$label".txt "$bin" >/tmp/lb-"$label".log 2>&1 &
    local pid=$!
    for _ in $(seq 1 60); do
        out=$(curl -sf -m 5 http://127.0.0.1:18200/ 2>/dev/null) && case "$out" in backend-*) break ;; esac
        sleep 0.5
    done
    case "$(curl -sf -m 5 http://127.0.0.1:18200/ 2>/dev/null)" in
        backend-*) ;; *) die "$label is not proxying" ;;
    esac
    # A fixed number of requests, not a fixed duration: the count per request
    # is the point, so the denominator has to be exact.
    wrk -t2 -c"$CONNECTIONS" -d10s http://127.0.0.1:18200/ >/tmp/wrk-"$label".out 2>&1
    local served
    served=$(awk '/requests in/ {print $1; exit}' /tmp/wrk-"$label".out)
    kill -INT "$pid" 2>/dev/null; sleep 3; pkill -f '/tmp/ae-lb' >/dev/null 2>&1; sleep 1

    say ""
    say "== $label: $served requests =="
    # strace ends with its own "total" row. Counting that as a syscall doubles
    # the total, which this printed as 20.14 per request against a real 10.07
    # until the row was excluded.
    awk -v r="${served:-1}" '
        /^[ ]*[0-9]/ && NF >= 4 && $NF != "total" {
            calls = $4 + 0; name = $NF
            if (calls > 0) { tot += calls; per[name] = calls / r }
        }
        END {
            printf "  %-20s %10s\n", "syscall", "per req"
            n = asorti(per, idx, "@val_num_desc")
            for (i = 1; i <= n && i <= 12; i++)
                if (per[idx[i]] >= 0.01) printf "  %-20s %10.2f\n", idx[i], per[idx[i]]
            printf "  %-20s %10.2f\n", "TOTAL", tot / r
        }' /tmp/strace-"$label".txt 2>/dev/null \
      || awk -v r="${served:-1}" '/^[ ]*[0-9]/ && NF>=4 && $NF != "total" { tot += $4 } END { printf "  total syscalls per request: %.2f\n", tot/r }' /tmp/strace-"$label".txt
}

[ -n "$AB_REF" ] && count_for /tmp/ae-lb-base "baseline"
count_for /tmp/ae-lb "current"
