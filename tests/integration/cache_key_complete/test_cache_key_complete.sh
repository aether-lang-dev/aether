#!/bin/sh
# Regression (#2538): a build cache key covers everything it names, or the
# build is not cached; long names are kept whole.
#
# 1. A module ten directories below the entry, so past the depth the cold
#    key's tree walk goes to, with the dependency manifest unwritable (a
#    directory in its slot), so the walk is all the key has. The walk
#    stopped at that depth without a word, the key never saw the module,
#    and an edit to it was served from the cache. A walk that cannot see
#    every file now makes no key.
# 2. The same deep project with a writable manifest is still cached: the
#    build that had no key asks aetherc for the manifest anyway, and
#    publishes under the key computed from it, so the next run hits.
# 3. Eight 250-byte --lib directories: the key text passed the 2 KB it was
#    built in, and the -D defines at its end were cut off, so a build with
#    another define was served the first one's binary.
# 4. Two functions whose reserved-namespace names (a leading `_`) share
#    their first 300 bytes: the rename to `ae_...` went through a 280-byte
#    buffer, and both became one C name.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] cache_key_complete: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

deep_project() {
    mkdir -p "$1/a/b/c/d/e/f/g/h/i"
    printf 'import a.b.c.d.e.f.g.h.i.deep\n\nmain() {\n    println(deep.value())\n}\n' \
        > "$1/main.ae"
    printf 'exports (value)\n\nvalue() -> string {\n    return "%s"\n}\n' "$2" \
        > "$1/a/b/c/d/e/f/g/h/i/deep.ae"
}

# --- 1. a module past the walk's depth, and no manifest ---
AETHER_CACHE_DIR="$TMP/cache1"
export AETHER_CACHE_DIR
deep_project "$TMP/p1" one
cd "$TMP/p1" || exit 1
got1=$("$AE" run main.ae 2>"$TMP/run1.err" | tr -d '\r')
dep=""
for d in "$AETHER_CACHE_DIR"/*.deps; do [ -f "$d" ] && dep="$d"; done
if [ "$got1" != "one" ] || [ -z "$dep" ]; then
    echo "  [FAIL] cache_key_complete: the first run printed '$got1' and left no manifest"
    sed 's/^/        /' "$TMP/run1.err" | head -5
    fail=1
else
    rm -f "$dep"
    mkdir "$dep"
    "$AE" run main.ae >/dev/null 2>&1
    printf 'exports (value)\n\nvalue() -> string {\n    return "two"\n}\n' \
        > "$TMP/p1/a/b/c/d/e/f/g/h/i/deep.ae"
    got=$("$AE" run main.ae 2>/dev/null | tr -d '\r')
    if [ "$got" != "two" ]; then
        echo "  [FAIL] cache_key_complete: an edit to a module 10 levels down was served from the cache ('$got')"
        fail=1
    else
        echo "  [PASS] cache_key_complete: an edit past the walk's depth rebuilds"
    fi
fi

# --- 2. the same project, manifest writable: still cached ---
AETHER_CACHE_DIR="$TMP/cache2"
export AETHER_CACHE_DIR
deep_project "$TMP/p2" three
cd "$TMP/p2" || exit 1
"$AE" run main.ae >/dev/null 2>&1
got=$("$AE" run --verbose main.ae 2>"$TMP/run2.err" | tr -d '\r' | tail -1)
if [ "$got" != "three" ] || ! grep -q "\[cache\] hit" "$TMP/run2.err"; then
    echo "  [FAIL] cache_key_complete: the second run of a deep project printed '$got' and did not hit the cache"
    grep "\[cache\]" "$TMP/run2.err" | sed 's/^/        /' | head -5
    fail=1
else
    echo "  [PASS] cache_key_complete: a deep project is cached from its manifest"
fi

# --- 3. a key past 2 KB: eight long --lib directories ---
AETHER_CACHE_DIR="$TMP/cache3"
export AETHER_CACHE_DIR
mkdir -p "$TMP/p3"
cd "$TMP/p3" || exit 1
cat > main.ae <<'AEOF'
main() {
    when defined(AE_KEY_A) { println("A") }
    when defined(AE_KEY_B) { println("B") }
}
AEOF
pad=$(i=0; while [ $i -lt 240 ]; do printf x; i=$((i + 1)); done)
set --
i=0
while [ $i -lt 8 ]; do set -- "$@" --lib "ae_key_lib_${i}_$pad"; i=$((i + 1)); done
gota=$("$AE" run "$@" -D AE_KEY_A main.ae 2>"$TMP/run3.err" | tr -d '\r')
gotb=$("$AE" run "$@" -D AE_KEY_B main.ae 2>>"$TMP/run3.err" | tr -d '\r')
if [ "$gota $gotb" != "A B" ]; then
    echo "  [FAIL] cache_key_complete: with a 2 KB key, -D AE_KEY_A then -D AE_KEY_B printed '$gota' and '$gotb'"
    sed 's/^/        /' "$TMP/run3.err" | head -5
    fail=1
else
    echo "  [PASS] cache_key_complete: the defines count in a key past 2 KB"
fi

# --- 4. two long reserved-namespace function names ---
# Read from the C aetherc writes: a call to a name this long is still cut
# where the call is emitted (a separate limit), so the program is not run.
mkdir -p "$TMP/p4"
cd "$TMP/p4" || exit 1
long=$(i=0; while [ $i -lt 300 ]; do printf a; i=$((i + 1)); done)
{
    printf '_%sx() -> int { return 1 }\n' "$long"
    printf '_%sy() -> int { return 2 }\n\n' "$long"
    printf 'main() {\n    println("main")\n}\n'
} > main.ae
if ! "$AETHERC" main.ae out.c >"$TMP/aetherc4.log" 2>&1; then
    echo "  [FAIL] cache_key_complete: aetherc failed on two 302-byte _names"
    sed 's/^/        /' "$TMP/aetherc4.log" | head -5
    fail=1
elif ! grep -q "ae_${long}x(" out.c || ! grep -q "ae_${long}y(" out.c; then
    echo "  [FAIL] cache_key_complete: two 302-byte _names sharing 300 bytes were not renamed whole"
    fail=1
else
    echo "  [PASS] cache_key_complete: two long _names sharing 300 bytes are renamed whole"
fi

exit $fail
