#!/bin/sh
# Regression (#1882): editing a PROJECT-ROOT module must invalidate the cache
# when the entry file sits in a SUBDIRECTORY.
#
# Module resolution is CWD-relative (aether_module.c "Try 3-6": src/<m>/module.ae,
# <m>/module.ae, <m>.ae, all probed from the process's cwd), so a project-root
# module resolves for an entry file anywhere. But the cache key hashed only the
# directory the ENTRY sits in (#1421) — the project root when you run
# `ae run main.ae`, and NOT when you run `ae run tests/suite.ae`, where it
# hashed tests/ and never saw the module that actually got compiled.
#
# `tests/<suite>.ae` importing a module from the project root is the ordinary
# layout for an Aether project's own test suite, so this sat on the default
# path: editing the module under test left the key unchanged and the suite
# re-ran the PREVIOUS binary. The failure mode is the bad one — a test suite
# reporting GREEN against code it never compiled.
#
# Asserts both directions, since a cache that never hits would also "pass" a
# staleness check while making every run slow.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] cache_subdir_entry_root_module: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/proj/greeter" "$TMP/proj/tests"
cd "$TMP/proj" || exit 1

write_greeter() {
    cat > greeter/module.ae <<AEOF
exports(greet)
greet() -> string {
    return "$1"
}
AEOF
}

cat > tests/test_it.ae <<'AEOF'
import greeter

main() {
    println(greeter.greet())
}
AEOF

run_sub() { "$AE" run tests/test_it.ae 2>&1 | tail -1; }

# --- 1. the entry-in-a-subdirectory case, which is the bug ---------------
write_greeter "V1"
got=$(run_sub)
[ "$got" = "V1" ] || { echo "  [FAIL] cache_subdir_entry_root_module: first run printed '$got', want V1"; exit 1; }

write_greeter "V2"
got=$(run_sub)
[ "$got" = "V2" ] || {
    echo "  [FAIL] cache_subdir_entry_root_module: after editing greeter/module.ae the"
    echo "         subdirectory entry printed '$got', want V2 — a stale cached binary"
    exit 1
}

# A second edit, so this cannot pass by invalidating exactly once.
write_greeter "V3"
got=$(run_sub)
[ "$got" = "V3" ] || { echo "  [FAIL] cache_subdir_entry_root_module: second edit printed '$got', want V3"; exit 1; }

# --- 2. the root-entry case must keep working (it always did) ------------
cp tests/test_it.ae ./root_entry.ae
write_greeter "R1"
got=$("$AE" run root_entry.ae 2>&1 | tail -1)
[ "$got" = "R1" ] || { echo "  [FAIL] cache_subdir_entry_root_module: root entry printed '$got', want R1"; exit 1; }
write_greeter "R2"
got=$("$AE" run root_entry.ae 2>&1 | tail -1)
[ "$got" = "R2" ] || { echo "  [FAIL] cache_subdir_entry_root_module: root entry after edit printed '$got', want R2"; exit 1; }

# --- 3. and the cache must still HIT when nothing changed ----------------
# Over-invalidating would pass every check above while making each run a full
# rebuild, so measure that an unchanged re-run is materially faster.
# The cache must still HIT when nothing changed. Over-invalidating would
# satisfy every check above while making each run a full rebuild.
#
# Asserted by COUNTING CACHE ENTRIES, not by timing. Two earlier attempts used
# a clock and both were wrong: an absolute "< 80ms" encoded the speed of the
# box it was written on (a slow macOS runner needed 159ms for a genuine hit),
# and a rebuild-vs-hit ratio was flaky because the "forced rebuild" could
# itself hit a warm entry built earlier in this test. A hit reuses an entry and
# a miss creates one, so the entry count answers the question exactly, on every
# machine, with no timing at all.
CACHE_DIR="${AETHER_CACHE_DIR:-$HOME/.aether/cache}"
count_entries() { ls -1 "$CACHE_DIR" 2>/dev/null | wc -l | tr -d ' '; }

# Unique per run: a fixed body would already have a cache entry from an
# earlier invocation of this test, so "did the count grow" would answer no for
# the wrong reason.
STAMP="S$$-$(date +%s)"
write_greeter "$STAMP"
"$AE" run tests/test_it.ae >/dev/null 2>&1          # populate
before=$(count_entries)
"$AE" run tests/test_it.ae >/dev/null 2>&1          # unchanged: must reuse
after=$(count_entries)
[ "$before" = "$after" ] || {
    echo "  [FAIL] cache_subdir_entry_root_module: an unchanged re-run added a cache"
    echo "         entry ($before -> $after) — the cache never hits, so every run rebuilds"
    exit 1
}

# And the converse, so the check above cannot pass by the cache being broken
# in the other direction: a real edit MUST add an entry.
write_greeter "${STAMP}-edited"
"$AE" run tests/test_it.ae >/dev/null 2>&1
edited=$(count_entries)
[ "$edited" -gt "$after" ] || {
    echo "  [FAIL] cache_subdir_entry_root_module: editing the module added no cache"
    echo "         entry ($after -> $edited) — the key did not change"
    exit 1
}

echo "  [PASS] cache_subdir_entry_root_module: a root module edit invalidates a subdir entry's cache, and the cache still hits"
