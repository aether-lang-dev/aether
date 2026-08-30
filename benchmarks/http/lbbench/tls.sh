#!/bin/bash
# The same comparison as run.sh, with the balancer terminating TLS.
#
# Most traffic a reverse proxy carries is TLS, and this path is not the one
# run.sh measures: the event driver takes plain-HTTP connections only, so a TLS
# listener falls back to a thread per connection. That is exactly the cost the
# driver exists to remove, and it had never been measured, which is why this
# exists before any attempt to fix it.
set -uo pipefail

. /bench/use_mounted.sh
lbbench_use_mounted tls.sh "$@"

DURATION="${DURATION:-20s}"
CONNECTIONS="${CONNECTIONS:-50}"
THREADS="${THREADS:-2}"
WARMUP="${WARMUP:-5s}"
GEN_CPUS="${GEN_CPUS:-0,1}"
LB_CPUS="${LB_CPUS:-2,3}"
ROUNDS="${ROUNDS:-4}"

say() { printf '%s\n' "$*" >&2; }
die() { say "ERROR: $*"; exit 1; }

command -v openssl >/dev/null 2>&1 || die "openssl is not in the image"

mkdir -p /bench/tls
if [ ! -f /bench/tls/cert.pem ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 2 \
        -subj "/CN=127.0.0.1" \
        -keyout /bench/tls/key.pem -out /bench/tls/cert.pem >/dev/null 2>&1 \
        || die "could not make a certificate"
fi

nginx -c /bench/backends.conf -p /tmp || die "backends did not start"
for p in 19001 19002; do
    for _ in $(seq 1 40); do curl -sf -o /dev/null "http://127.0.0.1:$p/" && break; sleep 0.25; done
    curl -sf -o /dev/null "http://127.0.0.1:$p/" || die "backend on $p never answered"
done

say "building aether ..."
cp -r /src /build && cd /build && rm -rf build
make -j"$(nproc)" >/tmp/build.log 2>&1 || { tail -20 /tmp/build.log >&2; die "build failed"; }
./build/ae build benchmarks/http/lb_reuse_lb.ae -o /tmp/ae-lb >/tmp/lbbuild.log 2>&1 \
    || { tail -20 /tmp/lbbuild.log >&2; die "lb build failed"; }

stop_lb() {
    pkill -f '/tmp/ae-lb' >/dev/null 2>&1
    nginx -c /bench/nginx-lb-tls.conf -p /tmp -s quit >/dev/null 2>&1
    pkill -x haproxy >/dev/null 2>&1
    sleep 1
    rm -f /tmp/lb-tls.pid
}

wait_pid_file() {
    local f="$1" i pid
    for i in $(seq 1 60); do
        pid=$(cat "$f" 2>/dev/null || true)
        if [ -n "$pid" ] && [ -d "/proc/$pid" ]; then echo "$pid"; return 0; fi
        sleep 0.1
    done
    return 1
}

# A balancer that answers errors quickly reads as fast, so nothing is reported
# for one that is not actually proxying over TLS.
verify() {
    local body
    body=$(curl -sk -m 5 https://127.0.0.1:18300/ 2>/dev/null) || return 1
    case "$body" in backend-*) return 0 ;; *) return 1 ;; esac
}

cpu_ticks() {                     # cpu_ticks <root-pid>
    local root="$1" tu=0 ts=0 pid line rest
    for pid in $root $(pgrep -P "$root" 2>/dev/null); do
        [ -r "/proc/$pid/stat" ] || continue
        line=$(cat "/proc/$pid/stat" 2>/dev/null) || continue
        rest=${line#*") "}
        tu=$((tu + $(printf '%s' "$rest" | awk '{print $12}')))
        ts=$((ts + $(printf '%s' "$rest" | awk '{print $13}')))
    done
    printf '%s' "$((tu + ts))"
}

CLK=$(getconf CLK_TCK 2>/dev/null || echo 100)

measure() {                       # measure <label> <pid>
    local label="$1" pid="$2"
    [ -n "$pid" ] && [ -d "/proc/$pid" ] || { say "  $label: no pid, skipped"; return; }
    for _ in $(seq 1 40); do verify && break; sleep 0.25; done
    verify || { say "  $label: not proxying over TLS, skipped"; return; }

    taskset -c "$GEN_CPUS" wrk -t"$THREADS" -c"$CONNECTIONS" -d"$WARMUP" \
        https://127.0.0.1:18300/ >/dev/null 2>&1

    local before after
    before=$(cpu_ticks "$pid")
    local out
    out=$(taskset -c "$GEN_CPUS" wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" \
          --latency https://127.0.0.1:18300/ 2>&1)
    after=$(cpu_ticks "$pid")

    local rps reqs p99
    rps=$(printf '%s' "$out" | awk '/Requests\/sec/ {print $2}')
    reqs=$(printf '%s' "$out" | awk '/requests in/ {print $1}')
    p99=$(printf '%s' "$out" | awk '/99%/ {print $2; exit}')
    [ -n "$reqs" ] || { say "  $label: no requests"; return; }

    awk -v l="$label" -v r="${rps:-0}" -v n="$reqs" -v d=$((after - before)) \
        -v clk="$CLK" -v p="${p99:-?}" 'BEGIN {
        printf "  %-9s %10.2f rps   p99 %-9s cpu %6.1f us/req\n",
               l, r, p, d * 1000000.0 / clk / n > "/dev/stderr"
    }'
}

printf '\n%s\n' "== TLS termination: $DURATION, $CONNECTIONS connections ==" >&2

for round in $(seq 1 "$ROUNDS"); do
    say "-- round $round of $ROUNDS"

    stop_lb
    BACKENDS="http://127.0.0.1:19001;http://127.0.0.1:19002" PORT=18300 \
        TLS_CERT=/bench/tls/cert.pem TLS_KEY=/bench/tls/key.pem \
        taskset -c "$LB_CPUS" /tmp/ae-lb >/tmp/ae-lb.log 2>&1 &
    measure aether "$!"

    stop_lb
    taskset -c "$LB_CPUS" nginx -c /bench/nginx-lb-tls.conf -p /tmp
    measure nginx "$(wait_pid_file /tmp/lb-tls.pid || true)"
done

stop_lb
say ""
say "Read the CPU per request. Throughput over TLS moves with the handshake"
say "rate, and wrk reuses connections, so it says less here than it does"
say "over cleartext."
