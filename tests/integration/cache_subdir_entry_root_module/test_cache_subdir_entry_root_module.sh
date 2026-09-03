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
write_greeter "S1"
"$AE" run tests/test_it.ae >/dev/null 2>&1          # populate
t0=$(date +%s%N 2>/dev/null || echo 0)
"$AE" run tests/test_it.ae >/dev/null 2>&1
t1=$(date +%s%N 2>/dev/null || echo 0)
if [ "$t0" != "0" ] && [ "$t1" != "0" ]; then
    ms=$(( (t1 - t0) / 1000000 ))
    # A real rebuild of even this trivial program is >100ms; a cache hit is
    # a handful. 80ms leaves generous headroom on a loaded CI runner.
    [ "$ms" -lt 80 ] || {
        echo "  [FAIL] cache_subdir_entry_root_module: an unchanged re-run took ${ms}ms;"
        echo "         the cache appears never to hit, so every run is a rebuild"
        exit 1
    }
fi

echo "  [PASS] cache_subdir_entry_root_module: a root module edit invalidates a subdir entry's cache, and the cache still hits"
