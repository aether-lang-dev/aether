#!/bin/sh
# Regression (#2500): the second identical `ae build` must hit the cache, and
# one program must leave one cache entry.
#
# The first build of a source has no depfile yet, so its key falls back to a
# walk of the source tree. aetherc writes the depfile during that build, and
# every later build keys on the dependencies it lists instead. The first build
# published its binary under the tree-walk key, which no later build computes:
# the second build missed, compiled again and published a second copy, and
# only the third hit. `ae run` already recomputed the key once the depfile was
# written; `ae build` now does too.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] cache_build_second_hit: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Counts the entries, so the cache must be this test's alone.
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR

mkdir -p "$TMP/proj"
cd "$TMP/proj" || exit 1
cat > helper.ae <<'AEOF'
greet(n: int) -> int {
    return n * 2 + 1
}
AEOF
cat > main.ae <<'AEOF'
import helper

main() {
    println("v=${helper.greet(20)}")
}
AEOF

build() {
    if ! "$AE" build main.ae -o "./$1" >"$TMP/build.log" 2>&1; then
        echo "  [FAIL] cache_build_second_hit: build failed"
        sed 's/^/        /' "$TMP/build.log" | head -10
        exit 1
    fi
    got=$("./$1" 2>&1 | tr -d '\r')
    if [ "$got" != "v=41" ]; then
        echo "  [FAIL] cache_build_second_hit: $1 printed '$got', expected 'v=41'"
        exit 1
    fi
}

# Binaries only: the depfile beside them is <hash>.deps.
entries() {
    ls "$AETHER_CACHE_DIR" 2>/dev/null | grep -cE '^[0-9a-f]{16}(\.exe)?$'
}

build out1
if grep -q "cache hit" "$TMP/build.log"; then
    echo "  [FAIL] cache_build_second_hit: the first build hit an empty cache"
    exit 1
fi

build out2
if ! grep -q "cache hit" "$TMP/build.log"; then
    echo "  [FAIL] cache_build_second_hit: the second identical build missed the cache"
    sed 's/^/        /' "$TMP/build.log" | head -5
    exit 1
fi

build out3
n=$(entries)
if [ "$n" != "1" ]; then
    echo "  [FAIL] cache_build_second_hit: three identical builds left $n cache entries, expected 1"
    ls "$AETHER_CACHE_DIR" | sed 's/^/        /'
    exit 1
fi

echo "  [PASS] cache_build_second_hit: the second identical build hits and one program leaves one entry"
