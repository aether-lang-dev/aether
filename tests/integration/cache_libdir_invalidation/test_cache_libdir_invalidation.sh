#!/bin/sh
# Regression: editing a module under the default lib/ directory must
# invalidate the build cache (#1025, #1235).
#
# The cache key folds in the content of every source file under each lib
# dir. On Windows that walk was compiled out entirely (only the lib dir's
# own mtime was hashed, which does not change on an edit-in-place), so
# every lib/ module edit served a stale cached binary until
# `ae cache clear`. The walk now has a native Windows implementation;
# this test runs on every platform, including the MSYS2 CI job.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

# Its own cache: the test decides whether an edit invalidated the key, which is
# only meaningful when nothing else is publishing entries into the same
# directory.
AETHER_CACHE_DIR="$TMPDIR/cache"
export AETHER_CACHE_DIR

mkdir -p "$TMPDIR/proj/lib/greet"
cd "$TMPDIR/proj"

cat > lib/greet/module.ae <<'EOF'
exports (hello)
hello() -> string { return "v1" }
EOF

cat > main.ae <<'EOF'
import greet
main() { println(greet.hello()) }
EOF

OUT1=$("$AE" run main.ae 2>/dev/null | tail -1)
if [ "$OUT1" != "v1" ]; then
    echo "  [FAIL] baseline run printed '$OUT1', expected v1"
    exit 1
fi

# Edit the module in place. Keep byte length identical (v1 -> v2) so an
# mtime+size key cannot pass by accident; only content hashing catches it.
sed 's/v1/v2/' lib/greet/module.ae > lib/greet/module.ae.new
mv lib/greet/module.ae.new lib/greet/module.ae

OUT2=$("$AE" run main.ae 2>/dev/null | tail -1)
if [ "$OUT2" != "v2" ]; then
    echo "  [FAIL] lib/ edit served stale binary: got '$OUT2', expected v2 (#1235)"
    exit 1
fi

# Same check through `ae build` with a fresh output name: the stale
# artifact must not be served under a different -o either.
sed 's/v2/v3/' lib/greet/module.ae > lib/greet/module.ae.new
mv lib/greet/module.ae.new lib/greet/module.ae

"$AE" build main.ae -o out3 >/dev/null 2>&1
OUT3=$(./out3 | tail -1)
if [ "$OUT3" != "v3" ]; then
    echo "  [FAIL] ae build served stale binary: got '$OUT3', expected v3 (#1235)"
    exit 1
fi

echo "  [PASS] cache_libdir_invalidation: lib/ edits invalidate run + build caches"
