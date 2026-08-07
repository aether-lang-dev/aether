#!/bin/sh
# std.http.proxy cache/retry/rate-limit/trace/metrics integration
# tests, part 2 of 2.
#
# Tests 9-16 live here: breaker-recovery, cache (× 3), retry,
# rate-limit, trace, metrics. The first 8 (RR/WRR/ip-hash/cookie-
# hash/drain/health/breaker-open) live in
# test_http_reverse_proxy_pool.sh — splitting the suite gives each
# half its own 180 s timeout budget on the slow Windows mingw CI
# runner, which the previous single-file form was tripping over.
#
# DESIGN — see part 1's header. Warm 3 upstreams + fresh proxy per
# subtest, counters from proxy's own /proxy-metrics, no `set -e`,
# polling on real observables, SIGKILL teardown.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if ! command -v curl >/dev/null 2>&1; then
    echo "  [SKIP] curl not on PATH"
    exit 0
fi

TMPDIR="$(mktemp -d)"
PIDS=""
PROXY_PID=""

cleanup() {
    if [ -n "$PROXY_PID" ]; then
        kill -9 "$PROXY_PID" 2>/dev/null || true
    fi
    for pid in $PIDS; do
        kill -9 "$pid" 2>/dev/null || true
    done
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

cd "$SCRIPT_DIR"
if ! AETHER_HOME="$ROOT" "$AE" build server.ae -o "$TMPDIR/server" >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] build:"; head -30 "$TMPDIR/build.log"; exit 1
fi

# ----- helpers (mirror of part 1) -----------------------------

start_proc() {
    role="$1"
    log="$TMPDIR/$role.log"
    "$TMPDIR/server" "$role" >"$log" 2>&1 &
    new_pid=$!
    disown "$new_pid" 2>/dev/null || true
    eval "PID_$role=\$new_pid"
}

wait_for_port() {
    role="$1"; port="$2"
    log="$TMPDIR/$role.log"
    pid=$(eval echo \$PID_$role)
    deadline=$(($(date +%s) + 15))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "  [FAIL] $role died:"; head -30 "$log"; exit 1
        fi
        if curl -s -o /dev/null --connect-timeout 0.3 --max-time 1 \
                "http://127.0.0.1:$port/health" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    echo "  [FAIL] $role never accepted on port $port:"; head -30 "$log"; exit 1
}

start_three_upstreams() {
    start_proc upstream_a; PIDS="$PIDS $PID_upstream_a"
    start_proc upstream_b; PIDS="$PIDS $PID_upstream_b"
    start_proc upstream_c; PIDS="$PIDS $PID_upstream_c"
    wait_for_port upstream_a 19101
    wait_for_port upstream_b 19102
    wait_for_port upstream_c 19103
}

# Non-fatal port wait: 0 if up, 1 if the process died or never bound.
wait_for_port_soft() {
    role="$1"; port="$2"
    pid=$(eval echo \$PID_$role)
    deadline=$(($(date +%s) + 15))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        kill -0 "$pid" 2>/dev/null || return 1
        if curl -s -o /dev/null --connect-timeout 0.3 --max-time 1 \
                "http://127.0.0.1:$port/health" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# Retry the proxy bind: kill -9 + immediate rebind on 19100 can briefly lose
# to a TIME_WAIT listener even with SO_REUSEADDR — transient, so retry.
start_proxy() {
    proxy_role="$1"
    attempt=0
    while [ "$attempt" -lt 5 ]; do
        start_proc "$proxy_role"
        PROXY_PID=$(eval echo \$PID_$proxy_role)
        if wait_for_port_soft "$proxy_role" 19100; then
            return 0
        fi
        kill -9 "$PROXY_PID" 2>/dev/null || true
        attempt=$((attempt + 1))
        sleep 0.3
    done
    echo "  [FAIL] $proxy_role never bound port 19100 after 5 attempts:"
    head -30 "$TMPDIR/$proxy_role.log"; exit 1
}

stop_proxy() {
    if [ -n "$PROXY_PID" ]; then
        kill -9 "$PROXY_PID" 2>/dev/null || true
        PROXY_PID=""
    fi
}

clear_upstream_b_503() {
    curl -s -o /dev/null --max-time 3 -X POST "http://127.0.0.1:19102/admin/200" 2>/dev/null || true
}

PROXY="http://127.0.0.1:19100"

# Comprehensive diagnostic snapshot on every failure — see part 1.
dump_diagnostics() {
    echo "  --- diagnostic snapshot ---"
    for r in upstream_a upstream_b upstream_c proxy; do
        case $r in
            proxy) pid="$PROXY_PID" ;;
            *)     pid=$(eval echo \$PID_$r) ;;
        esac
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            echo "  $r (pid=$pid): alive"
        else
            echo "  $r (pid=$pid): DEAD"
            log="$TMPDIR/$r.log"
            if [ "$r" = "proxy" ] && [ ! -f "$log" ]; then
                log=$(ls "$TMPDIR"/proxy_*.log 2>/dev/null | tail -1)
            fi
            if [ -n "$log" ] && [ -f "$log" ]; then
                echo "  $r log tail:"
                tail -15 "$log" 2>/dev/null | sed 's/^/    /'
            fi
        fi
    done
    echo "  --- live /proxy-metrics (counters/health/breaker/cache) ---"
    curl -s --max-time 3 "$PROXY/proxy-metrics" 2>/dev/null \
        | grep -E "requests_total|upstream_healthy|breaker_state|cache_hits" \
        | head -30 | sed 's/^/    /' || echo "    (proxy not responding)"
}

fail() { echo "  [FAIL] $1"; dump_diagnostics; exit 1; }

metric_2xx() {
    upstream_url="$1"
    for attempt in 1 2 3; do
        body=$(curl -s --max-time 3 "$PROXY/proxy-metrics" 2>/dev/null)
        v=$(printf '%s\n' "$body" \
            | grep -F "aether_proxy_upstream_requests_total{upstream=\"${upstream_url}\",class=\"2xx\"}" \
            | awk '{print $NF}')
        case "$v" in
            ''|*[!0-9]*) sleep 0.1; continue ;;
            *) printf '%s' "$v"; return 0 ;;
        esac
    done
    fail "metric_2xx $upstream_url returned non-integer after 3 attempts: '$v'"
}

# Total 2xx requests forwarded across all 3 upstreams. Used by the
# cache tests as the "did we hit upstream at all" signal.
metric_2xx_total() {
    a=$(metric_2xx "http://localhost:19101")
    b=$(metric_2xx "http://localhost:19102")
    c=$(metric_2xx "http://localhost:19103")
    echo $((a + b + c))
}

wait_for_breaker_state() {
    upstream_url="$1"; expected_state="$2"; deadline_sec="${3:-10}"
    deadline=$(($(date +%s) + deadline_sec))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        body=$(curl -s --max-time 2 "$PROXY/proxy-metrics" 2>/dev/null || true)
        if echo "$body" | grep -qF \
            "aether_proxy_upstream_breaker_state{upstream=\"${upstream_url}\"} ${expected_state}"; then
            return 0
        fi
        sleep 0.1
    done
    fail "$upstream_url breaker did not reach state=$expected_state within ${deadline_sec}s"
}

# Drive probe requests until the breaker for `upstream_url` returns
# to CLOSED (state=0). Breaker state transitions are LAZY: the
# proxy's admit() only re-evaluates the OPEN→HALF_OPEN timeout
# when a request actually arrives, so polling /proxy-metrics
# without traffic would never observe recovery.
wait_for_breaker_recovery() {
    upstream_url="$1"; deadline_sec="${2:-10}"
    deadline=$(($(date +%s) + deadline_sec))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        curl -s -o /dev/null --max-time 3 "$PROXY/echo" 2>/dev/null || true
        body=$(curl -s --max-time 2 "$PROXY/proxy-metrics" 2>/dev/null || true)
        if echo "$body" | grep -qF \
            "aether_proxy_upstream_breaker_state{upstream=\"${upstream_url}\"} 0"; then
            return 0
        fi
        sleep 0.1
    done
    fail "$upstream_url breaker did not recover within ${deadline_sec}s"
}

parallel_echo() {
    n="$1"
    URLS=""
    i=0
    while [ $i -lt $n ]; do
        URLS="$URLS $PROXY/echo"
        i=$((i + 1))
    done
    curl -Z -s -o /dev/null --max-time 5 $URLS >/dev/null 2>&1 || true
}

# Windows-reduced mode — see test_http_reverse_proxy_pool.sh for the
# rationale (MSYS2's bash fork-emulation + Defender + slower
# localhost = 10-100x per-curl overhead). Each curl-heavy subtest
# either gets a smaller iteration count or is skipped outright when
# its code path is already covered by another kept subtest. All
# reductions print a visible line in CI.
IS_WIN=0
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) IS_WIN=1 ;;
esac

# ----- spawn the warm upstreams ONCE ---------------------------
start_three_upstreams

# ---- Test 9: circuit breaker half-open recovery ------------
# Skipped on Windows: this exercises the breaker's open→half-open→
# closed transition. Test 8 in part 1 (breaker-open) already proves
# the open transition; recovery is symmetric and runs on POSIX.
if [ "$IS_WIN" = "1" ]; then
    echo "  [SKIP-WIN] Test 9 (breaker-recovery) — open path covered by Test 8"
else
    start_proxy proxy_breaker
    curl -s -o /dev/null --max-time 3 -X POST "http://127.0.0.1:19102/admin/503" 2>/dev/null || true
    parallel_echo 8
    wait_for_breaker_state "http://localhost:19102" 1 || exit 1
    curl -s -o /dev/null --max-time 3 -X POST "http://127.0.0.1:19102/admin/200" 2>/dev/null || true
    wait_for_breaker_recovery "http://localhost:19102" || exit 1
    B_AFTER_RECOVERY=$(metric_2xx "http://localhost:19102")
    i=0
    while [ $i -lt 9 ]; do
        curl -s -o /dev/null --max-time 3 "$PROXY/echo" 2>/dev/null || true
        i=$((i + 1))
    done
    B_FINAL=$(metric_2xx "http://localhost:19102")
    DELTA=$((B_FINAL - B_AFTER_RECOVERY))
    [ "$DELTA" -ge 2 ] || fail "breaker recovery: B delta=$DELTA expected ≥2"
    clear_upstream_b_503
    stop_proxy
fi

# ---- Test 10: cache hit — counter unchanged on second call --
# Windows: 2-req inner loop instead of 5. The hit-counter assertion
# is a strict equality (`delta == 0`), so two consecutive cached
# hits prove the same thing five do.
start_proxy proxy_cache
curl -s -o /dev/null --max-time 3 "$PROXY/echo_cacheable" 2>/dev/null || true
COUNT_BEFORE=$(metric_2xx_total)
if [ "$IS_WIN" = "1" ]; then CACHE_HIT_REQS=2; else CACHE_HIT_REQS=5; fi
i=0
while [ $i -lt $CACHE_HIT_REQS ]; do
    curl -s -o /dev/null --max-time 3 "$PROXY/echo_cacheable" 2>/dev/null || true
    i=$((i + 1))
done
COUNT_AFTER=$(metric_2xx_total)
DELTA=$((COUNT_AFTER - COUNT_BEFORE))
[ "$DELTA" = "0" ] || fail "cache hit: counter delta=$DELTA expected 0"
HDRS=$(curl -s -D - --max-time 3 -o /dev/null "$PROXY/echo_cacheable" 2>/dev/null)
echo "$HDRS" | grep -qi '^X-Cache: HIT' || fail "cache hit: missing X-Cache: HIT header"
stop_proxy

# ---- Test 11: cache TTL expiry ------------------------------
# Wall-clock dependency on `Cache-Control: max-age=1`. Sleep 1.2s
# = 1s TTL + 200ms slack; bounded by cache semantics we own, not
# CI runner speed.
start_proxy proxy_cache
curl -s -o /dev/null --max-time 3 "$PROXY/echo_cacheable_ttl" 2>/dev/null || true
COUNT_BEFORE=$(metric_2xx_total)
sleep 1.2
curl -s -o /dev/null --max-time 3 "$PROXY/echo_cacheable_ttl" 2>/dev/null || true
COUNT_AFTER=$(metric_2xx_total)
DELTA=$((COUNT_AFTER - COUNT_BEFORE))
[ "$DELTA" -ge 1 ] || fail "cache TTL: counter delta=$DELTA expected ≥1"
stop_proxy

# ---- Test 12: cache Vary — different Accept-Encoding = different entry --
start_proxy proxy_cache
curl -s -o /dev/null --max-time 3 -H 'Accept-Encoding: gzip' "$PROXY/echo_vary" 2>/dev/null || true
COUNT_BEFORE=$(metric_2xx_total)
curl -s -o /dev/null --max-time 3 -H 'Accept-Encoding: gzip' "$PROXY/echo_vary" 2>/dev/null || true
COUNT_AFTER_GZIP=$(metric_2xx_total)
[ "$COUNT_AFTER_GZIP" = "$COUNT_BEFORE" ] || fail "cache Vary: same key delta=$((COUNT_AFTER_GZIP - COUNT_BEFORE)) expected 0"
curl -s -o /dev/null --max-time 3 -H 'Accept-Encoding: identity' "$PROXY/echo_vary" 2>/dev/null || true
COUNT_AFTER_BOTH=$(metric_2xx_total)
[ $((COUNT_AFTER_BOTH - COUNT_AFTER_GZIP)) -ge 1 ] \
    || fail "cache Vary: identity didn't refetch (delta=$((COUNT_AFTER_BOTH - COUNT_AFTER_GZIP)))"
stop_proxy

# ---- Test 13: idempotent retry on 5xx ----------------------
# Windows: 3 requests instead of 9. The retry assertion is binary
# (each request must return a tagged success); three trials are
# enough to surface a regression where the retry logic falls back
# to surfacing the 5xx.
start_proxy proxy_retry
curl -s -o /dev/null --max-time 3 -X POST "http://127.0.0.1:19102/admin/503" 2>/dev/null || true
if [ "$IS_WIN" = "1" ]; then RETRY_REQS=3; else RETRY_REQS=9; fi
i=0
ALL_OK=1
while [ $i -lt $RETRY_REQS ]; do
    body=$(curl -s --max-time 5 "$PROXY/echo" 2>/dev/null)
    tag=$(echo "$body" | head -1 | cut -d: -f1)
    case "$tag" in
        tag=A|tag=B|tag=C) : ;;  # ok — proxy returned a tagged response
        *) ALL_OK=0 ;;
    esac
    i=$((i + 1))
done
[ "$ALL_OK" = "1" ] || fail "retry: not all $RETRY_REQS requests returned a tag= response"
clear_upstream_b_503
stop_proxy

# ---- Test 14: per-upstream rate limit (1 rps) -------------
start_proxy proxy_rate_limit
URLS=""
i=0
while [ $i -lt 12 ]; do
    URLS="$URLS $PROXY/echo"
    i=$((i + 1))
done
RESULTS=$(curl -Z -s -o /dev/null -w '%{http_code}\n' --max-time 3 $URLS 2>/dev/null)
SUCCESSES=$(printf '%s\n' "$RESULTS" | grep -c '^200$' || true)
FIVE_OH_THREES=$(printf '%s\n' "$RESULTS" | grep -c '^503$' || true)
[ "$SUCCESSES" -le 6 ] || fail "rate limit: $SUCCESSES successes (expected ≤6 with parallel arrivals)"
[ "$FIVE_OH_THREES" -ge 6 ] || fail "rate limit: only $FIVE_OH_THREES 503s (expected ≥6)"
stop_proxy

# ---- Test 15: W3C Trace-Context inject -------------------
start_proxy proxy_trace
INBOUND='00-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-bbbbbbbbbbbbbbbb-01'
RESP=$(curl -s --max-time 3 -H "traceparent: $INBOUND" "$PROXY/echo" 2>/dev/null)
echo "$RESP" | head -1 | grep -qE '^tag=[ABC]' || fail "trace passthrough: no tag in response"
stop_proxy

# ---- Test 16: Prometheus metrics endpoint ----------------
# Windows: 3-req warm-up instead of 6 — the assertion is "all five
# metric families are exposed", not a counter-value check.
start_proxy proxy_rr
if [ "$IS_WIN" = "1" ]; then METRICS_BATCH=3; else METRICS_BATCH=6; fi
parallel_echo $METRICS_BATCH
METRICS=$(curl -s --max-time 3 "$PROXY/proxy-metrics" 2>/dev/null)
echo "$METRICS" | grep -q 'aether_proxy_upstream_requests_total' || fail "metrics: requests_total absent"
echo "$METRICS" | grep -q 'aether_proxy_upstream_inflight'        || fail "metrics: inflight absent"
echo "$METRICS" | grep -q 'aether_proxy_upstream_healthy'         || fail "metrics: healthy absent"
echo "$METRICS" | grep -q 'aether_proxy_upstream_breaker_state'   || fail "metrics: breaker absent"
echo "$METRICS" | grep -q 'aether_proxy_cache_hits_total'         || fail "metrics: cache_hits absent"
echo "$METRICS" | grep -q '# TYPE'                                || fail "metrics: TYPE block absent"
stop_proxy

if [ "$IS_WIN" = "1" ]; then
    echo "  [PASS] http_reverse_proxy_pool_extra: 7/8 win-reduced — cache(3)/retry/rate-limit/trace/metrics"
else
    echo "  [PASS] http_reverse_proxy_pool_extra: 8/8 — breaker-recovery/cache(3)/retry/rate-limit/trace/metrics"
fi
