#!/bin/sh
# The build cache is bounded: AETHER_CACHE_MAX_MB (default 5120, 0 =
# unlimited), least-recently-used eviction, `ae cache gc`.
#
# The cache had no bound: every cold `ae run` / `ae build` added a static
# binary and nothing ever left, so a machine running the test sweep reached
# 26,000 entries and 12.7 GB with `ae cache clear` as the only remedy. A
# publish that takes the cache over the cap evicts the oldest-USED slots
# (a hit touches its slot) until it is under 90% of the cap; the slot just
# published is never evicted. The scan is rate-limited by a stamp file to
# once per ten minutes, so this test drives it with `ae cache gc`, which
# ignores the stamp.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] cache_size_limit: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0
export AETHER_HOME="$ROOT"
export AETHER_CACHE_DIR="$tmp/cache"

# Distinct programs, as many as it takes to overflow a 1 MB cap by a
# margin: N = 1 MB / one binary + 3 (each cached binary is ~100-200 KB).
gen() { printf 'main() { println("program %s") }\n' "$1" > "$tmp/p$1.ae"; }
run() { "$AE" run "$tmp/p$1.ae" >/dev/null 2>&1; }
cache_count() { "$AE" cache | sed -n 's/^Cache: \([0-9]*\) build.*/\1/p'; }

# 1. unlimited: nothing is evicted
export AETHER_CACHE_MAX_MB=0
gen 1; gen 2; gen 3
run 1; run 2; run 3
n="$(cache_count)"
if [ "$n" != "3" ]; then
    echo "  [FAIL] cache_size_limit: with AETHER_CACHE_MAX_MB=0 expected 3 builds, got '$n'"
    fail=1
fi
one="$(ls -l "$tmp/cache" | awk '$5 > 1000 {s=$5} END{print s}')"
[ -n "$one" ] && [ "$one" -gt 0 ] || { echo "  [FAIL] cache_size_limit: could not size a cached build"; exit 1; }
N=$(( 1024 * 1024 / one + 3 ))
i=4
while [ "$i" -le "$N" ]; do gen "$i"; run "$i"; i=$((i + 1)); done
n="$(cache_count)"
if [ "$n" != "$N" ]; then
    echo "  [FAIL] cache_size_limit: unlimited cache holds $n of $N builds"
    fail=1
fi

# 2. a 1 MB cap: gc evicts the least recently USED first. p1 is run again
#    just before, so p2 is the oldest and p1 the newest.
export AETHER_CACHE_MAX_MB=1
sleep 1
run 1
out="$("$AE" cache gc 2>&1)"
n="$(cache_count)"
if ! printf '%s\n' "$out" | grep -q "^Evicted [1-9]"; then
    echo "  [FAIL] cache_size_limit: gc evicted nothing over a 1 MB cap ($out)"
    fail=1
fi
if [ $((n * one)) -gt $((1024 * 1024)) ]; then
    echo "  [FAIL] cache_size_limit: after gc the cache still holds $n builds (~$((n * one)) bytes) over a 1 MB cap"
    fail=1
fi
if "$AE" run "$tmp/p2.ae" --verbose 2>&1 | grep -q "\[cache\] hit"; then
    echo "  [FAIL] cache_size_limit: the least-recently-used build (p2) survived eviction"
    fail=1
fi
if ! "$AE" run "$tmp/p1.ae" --verbose 2>&1 | grep -q "\[cache\] hit"; then
    echo "  [FAIL] cache_size_limit: the most-recently-used build (p1) was evicted"
    fail=1
fi

# 3. `ae cache` reports the limit
if ! "$AE" cache | grep -q "^Limit: 1 MB"; then
    echo "  [FAIL] cache_size_limit: ae cache does not report the limit"
    "$AE" cache | sed 's/^/        /'
    fail=1
fi

# 4. enforcement at publish time keeps the slot just published: refill
#    unlimited, drop the stamp so the next publish scans, then one cold
#    build under a 1 MB cap — it evicts older slots and the new build hits.
"$AE" cache clear >/dev/null 2>&1
export AETHER_CACHE_MAX_MB=0
i=1
while [ "$i" -le "$N" ]; do run "$i"; i=$((i + 1)); done
rm -f "$tmp/cache/gc.stamp"
export AETHER_CACHE_MAX_MB=1
gen 99
run 99
n="$(cache_count)"
if [ $((n * one)) -gt $((1024 * 1024)) ]; then
    echo "  [FAIL] cache_size_limit: a publish over the cap did not evict (cache holds $n builds)"
    fail=1
fi
if ! "$AE" run "$tmp/p99.ae" --verbose 2>&1 | grep -q "\[cache\] hit"; then
    echo "  [FAIL] cache_size_limit: the slot just published was evicted"
    fail=1
fi

# 5. a depfile not rewritten for 30 days goes with the scan; a fresh one stays
printf '# aether-deps v1
' > "$tmp/cache/old.deps"
printf '# aether-deps v1
' > "$tmp/cache/new.deps"
touch -d '40 days ago' "$tmp/cache/old.deps" 2>/dev/null || touch -t "$(date -d '40 days ago' +%Y%m%d%H%M 2>/dev/null || echo 202001010000)" "$tmp/cache/old.deps"
"$AE" cache gc >/dev/null 2>&1
if [ -f "$tmp/cache/old.deps" ] || [ ! -f "$tmp/cache/new.deps" ]; then
    echo "  [FAIL] cache_size_limit: stale depfile sweep (old present: $([ -f "$tmp/cache/old.deps" ] && echo yes || echo no), new present: $([ -f "$tmp/cache/new.deps" ] && echo yes || echo no))"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] cache_size_limit: unlimited keeps all, a 1 MB cap evicts least-recently-used first, ae cache reports the limit"
fi
exit $fail
