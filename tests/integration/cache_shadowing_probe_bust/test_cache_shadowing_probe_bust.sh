#!/bin/sh
# Regression (#1882, depfile cache key): a module dropped in at a resolution
# path an earlier `Try` probed-and-MISSED must invalidate the cache.
#
# The resolver probes higher-priority roots first (aether_module.c: Try 3
# `src/<m>/module.ae`, then Try 4 `src/<m>.ae`, …) and stops at the first hit.
# So a module resolving at Try 4 (`src/helper.ae`) has already probed-and-missed
# Try 3 (`src/helper/module.ae`). If someone later CREATES `src/helper/module.ae`,
# resolution flips to it — a real change in what gets compiled.
#
# A depfile that recorded only the files it OPENED would be byte-identical
# across that insert (the newly-created file was never opened before), so the
# warm key would not change and the cache would serve a build compiled against
# the shadowed module. The fix records the NEGATIVE probes too (paths looked
# for and not found); the once-absent path now existing flips the key. This
# test proves that: it is the one Nic required.
#
# Asserts both directions — the shadow must both take effect AND miss the cache;
# and an unrelated file appearing must NOT miss (or "always rebuild" would pass
# the staleness half while destroying the point of the cache).
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
if [ ! -x "$AE" ]; then echo "  [SKIP] cache_shadowing_probe_bust: ae not built"; exit 0; fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
# Isolated cache dir: the entry-count hit check needs this test to be the only
# writer (mirrors cache_subdir_entry_root_module).
AETHER_CACHE_DIR="$TMP/cache"; export AETHER_CACHE_DIR

mkdir -p "$TMP/proj/src"
cd "$TMP/proj"
cat > src/main.ae <<'AEOF'
import helper (help)
import std.io (println)
main() { println(help()) }
AEOF
cat > src/helper.ae <<'AEOF'
exports (help)
help() -> string { return "TRY4" }
AEOF

run_it() { "$AE" run src/main.ae 2>&1 | tail -1; }
count_entries() { "$AE" cache 2>/dev/null | sed -n 's/^Cache: *\([0-9][0-9]*\) build.*/\1/p'; }

# Warm the cache on the Try-4 resolution (two runs: cold writes the depfile,
# the second publishes/reads under the exact-deps key).
got=$(run_it); [ "$got" = "TRY4" ] || { echo "  [FAIL] cache_shadowing_probe_bust: first run printed '$got', want TRY4"; exit 1; }
run_it >/dev/null

# 1) An UNIMPORTED sibling .ae module changes -> MUST still hit. This is the
#    depfile's advantage over the old whole-tree hash: main.ae never imports
#    unused.ae, so editing it changes nothing that was compiled. The tree hash
#    folded every .ae under the entry dir and busted here; the depfile keys on
#    the actual import closure and does not. (A non-.ae file wouldn't
#    distinguish them — the tree walk ignores those anyway.)
cat > src/unused.ae <<'AEOF'
exports (unused)
unused() -> int { return 1 }
AEOF
before=$(count_entries); run_it >/dev/null; after=$(count_entries)
[ "$before" = "$after" ] || {
    echo "  [FAIL] cache_shadowing_probe_bust: editing an UNIMPORTED sibling module busted"
    echo "         the cache ($before -> $after entries) — the depfile keys on the import"
    echo "         closure, so an unimported .ae must not invalidate"; exit 1; }
cat > src/unused.ae <<'AEOF'
exports (unused)
unused() -> int { return 2 }
AEOF
before=$(count_entries); run_it >/dev/null; after=$(count_entries)
[ "$before" = "$after" ] || {
    echo "  [FAIL] cache_shadowing_probe_bust: editing an unimported sibling still busts"
    echo "         ($before -> $after) — whole-tree hashing, not import-closure keying"; exit 1; }

# 2) Shadow the Try-4 module with a Try-3 one -> MUST take effect AND miss.
mkdir -p src/helper
cat > src/helper/module.ae <<'AEOF'
exports (help)
help() -> string { return "TRY3-SHADOW" }
AEOF
before=$(count_entries)
got=$(run_it)
after=$(count_entries)
[ "$got" = "TRY3-SHADOW" ] || {
    echo "  [FAIL] cache_shadowing_probe_bust: after dropping src/helper/module.ae the"
    echo "         program printed '$got', want TRY3-SHADOW — a stale binary built against"
    echo "         the shadowed src/helper.ae (the recorded negative probe did not bust)"; exit 1; }
[ "$after" -gt "$before" ] || {
    echo "  [FAIL] cache_shadowing_probe_bust: the shadow resolved but no new cache entry"
    echo "         was created ($before -> $after) — the key did not change on the insert"; exit 1; }

echo "  [PASS] cache_shadowing_probe_bust: a module at a probed-absent path busts the cache; unrelated files do not"
exit 0
