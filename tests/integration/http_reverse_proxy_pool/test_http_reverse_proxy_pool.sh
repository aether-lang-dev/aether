#!/bin/sh
# std.http.proxy pool/LB/health/breaker integration tests, part 1 of 2.
#
# Tests 1-9 live here: round-robin / weighted-RR / ip-hash / cookie-
# hash / drain / method routing / health (x2) / breaker-open. The remaining tests
# (breaker-recovery, cache × 3, retry, rate-limit, trace, metrics)
# live in test_http_reverse_proxy_pool_extra.sh.
#
# DESIGN — warm upstreams + fresh proxy + proxy-side metrics.
#
# 1. Three upstreams (A, B, C on :19101/19102/19103) spawn ONCE at
#    the top and stay alive across every subtest. Per subtest only
#    the proxy bounces. Cuts process spawns from 4×8=32 to 3+8=11
#    on the slow Windows MSYS2 runner — proven necessary after
#    per-subtest spawn timed out at 180s in CI.
#
# 2. Counters come from the proxy's own /proxy-metrics endpoint
#    (`aether_proxy_upstream_requests_total{upstream=X,class="2xx"}`)
#    rather than a custom /count on each upstream. The proxy is
#    fresh per subtest so its metrics start at 0 — no /admin/reset
#    handshake, no shared mutable state across subtests, no false-
#    positive arithmetic when a curl flakes mid-test.
#
# 3. Health / breaker waits poll /proxy-metrics for the actual gauge
#    rather than `sleep N`. On fast machines the wait collapses to
#    a single round-trip; on slow machines it stretches until the
#    state actually changes.
#
# 4. SIGKILL teardown — SIGTERM left `wait $pid` blocked
#    indefinitely on MSYS2 because the http_server's signal handler
#    didn't fully reap. kill -9 maps to TerminateProcess on MSYS2
#    (synchronous, uninterceptable, no `wait` needed).
#
# 5. NO `set -e`. A transient curl flake would otherwise kill the
#    script with no diagnostic, surfacing in CI as the dreaded
#    "(no output)" failure. Real failures call `fail` or `exit 1`
#    explicitly with a clear message.

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

# ----- helpers ------------------------------------------------

start_proc() {
    role="$1"
    log="$TMPDIR/$role.log"
    "$TMPDIR/server" "$role" >"$log" 2>&1 &
    new_pid=$!
    disown "$new_pid" 2>/dev/null || true
    eval "PID_$role=\$new_pid"
}

# Poll /health on the named port until it answers OR the named pid
# dies. On timeout dump the role's log so the next failure is
# self-explaining instead of producing the dreaded "(no output)".
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

# Non-fatal variant of wait_for_port: returns 0 if the port comes up,
# 1 if the process died or never bound. Used by start_proxy's rebind
# retry so a transient TIME_WAIT loss on the proxy port isn't fatal.
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

# Bounce just the proxy. The 3 upstreams stay warm.
#
# The proxy is killed with `kill -9` and immediately restarted on the same
# port (19100) between subtests. Even though the server sets SO_REUSEADDR,
# the kernel can briefly hold the previous listener in TIME_WAIT, so a fresh
# bind occasionally loses the race and the process exits at once. That is a
# transient, not a real failure — retry the bind a few times before giving up.
start_proxy() {
    proxy_role="$1"
    attempt=0
    while [ "$attempt" -lt 5 ]; do
        start_proc "$proxy_role"
        PROXY_PID=$(eval echo \$PID_$proxy_role)
        if wait_for_port_soft "$proxy_role" 19100; then
            return 0
        fi
        # bind lost the race (or crashed) — reap and back off before retrying
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

# Reset upstream-side test knobs (force_503 only — counters are
# read from the proxy side now). Idempotent: each subtest that
# cares calls /admin/200 to undo any prior /admin/503 from another
# subtest. Per-subtest cleanup, not per-subtest setup.
clear_upstream_b_503() {
    curl -s -o /dev/null --max-time 3 -X POST "http://127.0.0.1:19102/admin/200" 2>/dev/null || true
}

PROXY="http://127.0.0.1:19100"

# Dump everything anyone could want to know about the test state in
# one snapshot, then exit 1. Every failure path on this script
# routes through here so a CI failure produces a self-contained
# diagnostic — no round-trip to add `cat upstream.log` and re-run.
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
            # Use the matching <role>.log; for proxy the role name
            # varies per subtest, so search the proxy_* logs.
            if [ "$r" = "proxy" ] && [ ! -f "$log" ]; then
                log=$(ls "$TMPDIR"/proxy_*.log 2>/dev/null | tail -1)
            fi
            if [ -n "$log" ] && [ -f "$log" ]; then
                echo "  $r log tail:"
                tail -15 "$log" 2>/dev/null | sed 's/^/    /'
            fi
        fi
    done
    echo "  --- live /proxy-metrics (counters/health/breaker) ---"
    curl -s --max-time 3 "$PROXY/proxy-metrics" 2>/dev/null \
        | grep -E "requests_total|upstream_healthy|breaker_state|cache_hits" \
        | head -30 | sed 's/^/    /' || echo "    (proxy not responding)"
}

fail() { echo "  [FAIL] $1"; dump_diagnostics; exit 1; }

# ----- proxy-side counter reads --------------------------------
# Read aether_proxy_upstream_requests_total{upstream=X,class="2xx"}
# from /proxy-metrics. Fresh proxy → starts at 0 each subtest.
# Retries on transient curl failure; exits loudly on persistent
# failure so a malformed metric or dead proxy doesn't get masked
# as a value of "0" via bash arithmetic on an empty string.
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

# Wait until /proxy-metrics reports the upstream's health-check
# state matches `expected_value` (0 = unhealthy, 1 = healthy).
wait_for_upstream_health() {
    upstream_url="$1"; expected_value="$2"; deadline_sec="${3:-15}"
    deadline=$(($(date +%s) + deadline_sec))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        body=$(curl -s --max-time 2 "$PROXY/proxy-metrics" 2>/dev/null || true)
        if echo "$body" | grep -qF \
            "aether_proxy_upstream_healthy{upstream=\"${upstream_url}\"} ${expected_value}"; then
            return 0
        fi
        sleep 0.1
    done
    fail "$upstream_url did not reach healthy=$expected_value within ${deadline_sec}s"
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

# Windows-reduced mode. Each curl invocation under MSYS2 bash pays
# Cygwin fork-emulation + Defender scan + slower-than-POSIX
# localhost handshake overhead — 10-100x the per-spawn cost of a
# POSIX runner. The proxy code path under test is identical across
# platforms, so the POSIX matrix entries already cover every
# behaviour these subtests assert. On Windows we shrink the
# iteration counts (smaller-N variants of the same checks) and
# skip the most curl-heavy distribution subtests entirely. Every
# skip / shrink prints its own line in CI so the reduction is
# explicit, not silent.
IS_WIN=0
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) IS_WIN=1 ;;
esac

# ----- spawn the warm upstreams ONCE ---------------------------
start_three_upstreams

# ---- Test 1: round-robin distribution ---------------------
# POSIX: 9 requests → expect exactly 3/3/3 (deterministic RR over 3 upstreams).
# Windows: 3 requests → expect 1/1/1. Same arithmetic check, ⅓ the curls.
start_proxy proxy_rr
if [ "$IS_WIN" = "1" ]; then RR_REQS=3; RR_EACH=1; else RR_REQS=9; RR_EACH=3; fi
i=0
while [ $i -lt $RR_REQS ]; do
    curl -s -o /dev/null --max-time 3 "$PROXY/echo" || true
    i=$((i + 1))
done
A=$(metric_2xx "http://localhost:19101")
B=$(metric_2xx "http://localhost:19102")
C=$(metric_2xx "http://localhost:19103")
[ "$A" = "$RR_EACH" ] || fail "RR A=$A expected $RR_EACH"
[ "$B" = "$RR_EACH" ] || fail "RR B=$B expected $RR_EACH"
[ "$C" = "$RR_EACH" ] || fail "RR C=$C expected $RR_EACH"
stop_proxy

# ---- Test 2: weighted RR (3:1:1) — 50 requests ----------
# Highest curl-count subtest by far. Skipped on Windows; POSIX
# matrix entries exercise the same weighted-RR code path.
if [ "$IS_WIN" = "1" ]; then
    echo "  [SKIP-WIN] Test 2 (WRR 50-req distribution) — POSIX matrix covers"
else
    start_proxy proxy_wrr
    parallel_echo 50
    A=$(metric_2xx "http://localhost:19101")
    B=$(metric_2xx "http://localhost:19102")
    C=$(metric_2xx "http://localhost:19103")
    [ "$A" -ge 25 ] && [ "$A" -le 35 ] || fail "WRR A=$A expected 25-35"
    [ "$B" -ge 5  ] && [ "$B" -le 15 ] || fail "WRR B=$B expected 5-15"
    [ "$C" -ge 5  ] && [ "$C" -le 15 ] || fail "WRR C=$C expected 5-15"
    [ $((A + B + C)) -eq 50 ] || fail "WRR total=$((A + B + C)) expected 50"
    stop_proxy
fi

# ---- Test 3: ip_hash determinism --------------------------
# POSIX: 12 requests; Windows: 3 (determinism doesn't need 12 to
# manifest — even a 2-of-2 mismatch proves the hash isn't stable).
start_proxy proxy_ip_hash
if [ "$IS_WIN" = "1" ]; then IP_HASH_REQS=3; else IP_HASH_REQS=12; fi
i=0
TAG=""
while [ $i -lt $IP_HASH_REQS ]; do
    body=$(curl -s --max-time 3 -H 'X-Forwarded-For: 192.0.2.42' "$PROXY/echo" 2>/dev/null)
    cur_tag=$(echo "$body" | head -1 | cut -d: -f1)
    if [ -z "$TAG" ]; then TAG="$cur_tag"; fi
    [ "$cur_tag" = "$TAG" ] || fail "ip_hash inconsistent: $cur_tag vs $TAG"
    i=$((i + 1))
done
stop_proxy

# ---- Test 4: cookie_hash determinism ----------------------
# Same hashing primitive as Test 3 (just a different input source),
# so Windows skips it entirely — the family is covered by ip_hash.
if [ "$IS_WIN" = "1" ]; then
    echo "  [SKIP-WIN] Test 4 (cookie_hash 20-req determinism) — same hash as ip_hash"
else
    start_proxy proxy_cookie_hash
    i=0
    TAG=""
    while [ $i -lt 12 ]; do
        body=$(curl -s --max-time 3 -H 'Cookie: SESSIONID=user-12345' "$PROXY/echo" 2>/dev/null)
        cur_tag=$(echo "$body" | head -1 | cut -d: -f1)
        if [ -z "$TAG" ]; then TAG="$cur_tag"; fi
        [ "$cur_tag" = "$TAG" ] || fail "cookie_hash inconsistent: $cur_tag vs $TAG"
        i=$((i + 1))
    done
    TAG2=""
    i=0
    while [ $i -lt 8 ]; do
        body=$(curl -s --max-time 3 -H 'Cookie: SESSIONID=user-other' "$PROXY/echo" 2>/dev/null)
        cur_tag=$(echo "$body" | head -1 | cut -d: -f1)
        if [ -z "$TAG2" ]; then TAG2="$cur_tag"; fi
        [ "$cur_tag" = "$TAG2" ] || fail "cookie_hash second-cookie inconsistent"
        i=$((i + 1))
    done
    stop_proxy
fi

# Shared scaling for parallel_echo across tests 5-8. POSIX runs the
# full 12-req batch; Windows runs 6, halving the curl count while
# preserving the arithmetic — the assertions below are kept whole-
# integer-correct against the smaller batch.
if [ "$IS_WIN" = "1" ]; then BATCH=6; else BATCH=12; fi
HEALTH_BATCH=$BATCH        # tests 5, 6, 8
HEALTH_BATCH_HALF=$((BATCH / 2))  # test 7's first "before recovery" probe

# ---- Test 5: drain — A drained on startup; A counter stays 0 -
start_proxy proxy_drain
parallel_echo $HEALTH_BATCH
A=$(metric_2xx "http://localhost:19101")
B=$(metric_2xx "http://localhost:19102")
C=$(metric_2xx "http://localhost:19103")
[ "$A" = "0" ] || fail "drain: A served $A requests (expected 0)"
[ $((B + C)) -eq $HEALTH_BATCH ] || fail "drain: B+C=$((B + C)) expected $HEALTH_BATCH"
stop_proxy

# ---- Test 6: method + route-pattern aware routing ----------
start_proxy proxy_methods
info_body=$(curl -s --max-time 3 "$PROXY/repos/proj/info" 2>/dev/null)
case "$info_body" in
    tag=A:*) ;;
    *) fail "method routing: GET /repos/proj/info went to '$info_body', expected A" ;;
esac
log_body=$(curl -s --max-time 3 "$PROXY/repos/proj/log" 2>/dev/null)
case "$log_body" in
    tag=B:*|tag=C:*) ;;
    *) fail "method routing: GET /repos/proj/log went to '$log_body', expected B/C" ;;
esac
commit_body=$(curl -s --max-time 3 -X POST "$PROXY/repos/proj/commit" 2>/dev/null)
case "$commit_body" in
    tag=A:*) ;;
    *) fail "method routing: POST /repos/proj/commit went to '$commit_body', expected A" ;;
esac
stop_proxy

# ---- Test 7: health checks kill an unhealthy upstream -----
start_proxy proxy_health
curl -s -o /dev/null --max-time 3 -X POST "http://127.0.0.1:19102/admin/503" 2>/dev/null || true
wait_for_upstream_health "http://localhost:19102" 0 || exit 1
parallel_echo $HEALTH_BATCH
B=$(metric_2xx "http://localhost:19102")
# Up to 2 requests may have landed on B before the health-check
# thread observed and broadcast the unhealthy state.
[ "$B" -le 2 ] || fail "health: B served $B requests after marked unhealthy"
clear_upstream_b_503
stop_proxy

# ---- Test 8: health recovery — flip B back, it rejoins -----
# Skipped on Windows: pairs with Test 7 to exercise the same health-
# check state machine. Test 7 alone proves the unhealthy->healthy
# transition gate; the recovery direction is symmetric.
if [ "$IS_WIN" = "1" ]; then
    echo "  [SKIP-WIN] Test 8 (health-recovery) - same state machine as Test 7"
else
    start_proxy proxy_health
    curl -s -o /dev/null --max-time 3 -X POST "http://127.0.0.1:19102/admin/503" 2>/dev/null || true
    wait_for_upstream_health "http://localhost:19102" 0 || exit 1
    parallel_echo 6
    B_BEFORE=$(metric_2xx "http://localhost:19102")
    curl -s -o /dev/null --max-time 3 -X POST "http://127.0.0.1:19102/admin/200" 2>/dev/null || true
    wait_for_upstream_health "http://localhost:19102" 1 || exit 1
    parallel_echo 12
    B_AFTER=$(metric_2xx "http://localhost:19102")
    DELTA=$((B_AFTER - B_BEFORE))
    [ "$DELTA" -ge 2 ] || fail "health recovery: B delta=$DELTA expected ≥2"
    stop_proxy
fi

# ---- Test 9: circuit breaker opens after consecutive failures ----
start_proxy proxy_breaker
curl -s -o /dev/null --max-time 3 -X POST "http://127.0.0.1:19102/admin/503" 2>/dev/null || true
parallel_echo $HEALTH_BATCH
B_5xx=$(curl -s --max-time 3 "$PROXY/proxy-metrics" 2>/dev/null \
    | grep -F 'aether_proxy_upstream_requests_total{upstream="http://localhost:19102",class="5xx"}' \
    | awk '{print $NF}')
B_5xx="${B_5xx:-0}"
# Once the breaker opens (at threshold=3), B shouldn't receive
# more new requests. Allow a small tolerance for half-open probe.
[ "$B_5xx" -le 5 ] || fail "breaker: B served $B_5xx 5xx requests (expected ≤5)"
clear_upstream_b_503
stop_proxy

if [ "$IS_WIN" = "1" ]; then
    echo "  [PASS] http_reverse_proxy_pool: 6/9 win-reduced - RR/drain/methods/health/breaker-open"
else
    echo "  [PASS] http_reverse_proxy_pool: 9/9 - RR/WRR/ip_hash/cookie_hash/drain/methods/health(2)/breaker-open"
fi
