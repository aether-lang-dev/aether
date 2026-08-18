#!/bin/sh
# Regression: aether.toml's [build] cflags must invalidate the build cache.
#
# The cache key covered the source tree, the -D symbols and --trace, but not
# the manifest's toolchain flags, which go straight onto the gcc line. Editing
# `cflags` therefore printed "Built (cache hit)" and served the binary built
# with the OLD flags. It was found staging an ASan workspace over an
# already-built tree: the build reported success and handed back the
# uninstrumented binary, so the sanitizer run measured nothing.
#
# Asserts both directions: a flag change must rebuild, and an unchanged build
# must still hit (a cache that never hits would pass a staleness check while
# making every build slow).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] cache_build_flags: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/proj"
cd "$TMP/proj" || exit 1

cat > shim.c <<'COF'
int probe_flag(void) {
#ifdef AE_CACHE_FLAG_PROBE
    return AE_CACHE_FLAG_PROBE;
#else
    return 0;
#endif
}
COF

cat > main.ae <<'AEOF'
extern probe_flag() -> int

main() {
    n = probe_flag()
    println("flag=${n}")
}
AEOF

write_toml() {
    cat > aether.toml <<TOF
[project]
name = "cacheflags"
version = "0.0.0"

[[bin]]
name = "app"
path = "main.ae"
extra_sources = ["shim.c"]

[build]
cflags = "-DAE_CACHE_FLAG_PROBE=$1"
TOF
}

build() {
    if ! "$AE" build main.ae -o ./app >"$TMP/build.log" 2>&1; then
        echo "  [FAIL] cache_build_flags: build failed"
        sed 's/^/        /' "$TMP/build.log" | head -10
        exit 1
    fi
}

write_toml 1
build
got=$(./app 2>&1)
if [ "$got" != "flag=1" ]; then
    echo "  [FAIL] cache_build_flags: first build printed '$got', expected 'flag=1'"
    exit 1
fi

# The bug: only the manifest's cflags change.
write_toml 2
build
got=$(./app 2>&1)
if [ "$got" != "flag=2" ]; then
    echo "  [FAIL] cache_build_flags: stale binary after editing [build] cflags"
    echo "         printed '$got', expected 'flag=2'"
    exit 1
fi

# The other direction: nothing changed, so this must be served from the cache.
build
if ! grep -q "cache hit" "$TMP/build.log"; then
    echo "  [FAIL] cache_build_flags: an unchanged rebuild did not hit the cache"
    sed 's/^/        /' "$TMP/build.log" | head -5
    exit 1
fi

echo "  [PASS] cache_build_flags: a [build] cflags edit rebuilds, an unchanged build hits"
