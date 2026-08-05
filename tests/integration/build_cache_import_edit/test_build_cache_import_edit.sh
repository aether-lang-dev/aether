#!/bin/sh
# Regression (#1421): editing an imported module must invalidate the build cache.
#
# The cache key hashed the entry file's content and the lib dirs, but a
# project's own sibling modules are neither: `import helper` next to
# `src/main.ae` resolves to `src/helper.ae`, which lived in no lib dir. Editing
# it left the key unchanged, so `ae build` printed "Built (cache hit)" and
# served a binary built from the OLD module. Deleting `target/` did not help,
# because the cache lives under ~/.aether/cache.
#
# That is the worst shape a cache bug can take: wrong output, successful
# report, and every measurement taken against it quietly invalid.
#
# Asserts both directions, since a cache that never hits would also "pass" a
# staleness check while making every build slow.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] build_cache_import_edit: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/proj/src"
cd "$TMP/proj" || exit 1
printf '[[bin]]\nname = "app"\npath = "src/main.ae"\n' > aether.toml
cat > src/main.ae <<'AEOF'
import std.io
import helper

main() {
    println(helper.greet())
}
AEOF

write_helper() {
    cat > src/helper.ae <<AEOF
greet() -> string {
    return "$1"
}
AEOF
}

build_and_read() {
    if ! "$AE" build src/main.ae -o ./app >"$TMP/build.log" 2>&1; then
        echo "  [FAIL] build_cache_import_edit: build failed"
        sed 's/^/        /' "$TMP/build.log" | head -10
        exit 1
    fi
    ./app 2>&1
}

write_helper "VERSION-ONE"
got=$(build_and_read)
if [ "$got" != "VERSION-ONE" ]; then
    echo "  [FAIL] build_cache_import_edit: first build printed '$got'"
    exit 1
fi

# The bug: only the imported module changes.
write_helper "VERSION-TWO"
got=$(build_and_read)
if [ "$got" != "VERSION-TWO" ]; then
    echo "  [FAIL] build_cache_import_edit: stale binary after editing an imported module"
    echo "         printed '$got', expected 'VERSION-TWO'"
    grep -q 'cache hit' "$TMP/build.log" && echo "         (the build reported a cache hit)"
    exit 1
fi

# Once more, so a fix that merely invalidates once is not enough.
write_helper "VERSION-THREE"
got=$(build_and_read)
if [ "$got" != "VERSION-THREE" ]; then
    echo "  [FAIL] build_cache_import_edit: stale on the second module edit ('$got')"
    exit 1
fi

# The other direction: an unchanged rebuild must still hit the cache, or the
# fix has simply disabled caching.
"$AE" build src/main.ae -o ./app >"$TMP/hit.log" 2>&1
if ! grep -q 'cache hit' "$TMP/hit.log"; then
    echo "  [FAIL] build_cache_import_edit: unchanged rebuild no longer hits the cache"
    sed 's/^/        /' "$TMP/hit.log" | head -5
    exit 1
fi

echo "  [PASS] build_cache_import_edit: module edits invalidate, unchanged rebuilds still hit"
