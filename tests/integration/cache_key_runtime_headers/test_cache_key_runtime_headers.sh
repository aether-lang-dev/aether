#!/bin/sh
# #2707: in a source tree, the build cache key covers the runtime and std
# headers that generated C includes. Editing one and running `make` left the
# key unchanged, so `ae run` served the binary built against the old header
# and a test reported the old code's result.
#
# The test runs `ae` from a source tree of its own: the built ae, aetherc and
# libaether.a under build/, and the runtime, std and include headers, copied
# into a temporary directory (ae counts as running from a source tree when
# aetherc sits beside it and runtime/ beside build/). A probe header is added
# there, changed once and removed; each change has to move the key, and the
# removal has to bring the original key back. Never the shared tree: the
# sweep runs other cache tests at the same time, and a header appearing in
# the repository would move their keys too.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/portable.sh"

fail() { echo "  [FAIL] cache_key_runtime_headers: $1"; exit 1; }

EXE=""
[ -f "$ROOT/build/ae.exe" ] && EXE=".exe"
if [ ! -f "$ROOT/build/ae$EXE" ] || [ ! -f "$ROOT/build/aetherc$EXE" ] ||
   [ ! -f "$ROOT/build/libaether.a" ] || [ ! -d "$ROOT/runtime" ]; then
    echo "  [SKIP] cache_key_runtime_headers: not a built source tree"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM
TREE="$TMP/tree"
mkdir -p "$TREE/build" || fail "could not make the tree"
for f in "ae$EXE" "aetherc$EXE" libaether.a aether_manifest.o; do
    [ -f "$ROOT/build/$f" ] && { cp "$ROOT/build/$f" "$TREE/build/" || fail "could not copy build/$f"; }
done
# The headers, in one process (each process costs tens of milliseconds on
# Windows).
(cd "$ROOT" && find runtime std include -name '*.h' 2>/dev/null | copy_listed "$TREE") ||
    fail "could not copy the headers"

AE="$TREE/build/ae$EXE"
probe="$TREE/runtime/utils/zz_cache_key_probe.h"
export AETHER_CACHE_DIR="$TMP/cache"
unset AETHER_HOME
printf 'main() {\n    println("probe")\n}\n' > "$TMP/p.ae"

# The key a build looks up (`[cache] hit: <key>` or `[cache] miss: <key>`
# under -v). awk, not a sed alternation: `\|` in a basic regex is a GNU sed
# extension, and macOS sed matched nothing.
key() {
    "$AE" build -v "$TMP/p.ae" -o "$TMP/p" 2>&1 |
        awk '$1 == "[cache]" && ($2 == "hit:" || $2 == "miss:") { print $3; exit }'
}

# A first build writes the depfile later keys are made from.
"$AE" build "$TMP/p.ae" -o "$TMP/p" > "$TMP/first.log" 2>&1 ||
    { tail -5 "$TMP/first.log"; fail "the copied tree could not build a program"; }
k1="$(key)"
[ -n "$k1" ] || fail "no cache key reported"
[ "$(key)" = "$k1" ] || fail "the key is not stable between two builds"

printf '/* cache key probe */\n' > "$probe"
k2="$(key)"
[ "$k2" != "$k1" ] || fail "adding a runtime header left the key unchanged"

printf '/* cache key probe, edited */\n' > "$probe"
k3="$(key)"
[ "$k3" != "$k2" ] || fail "editing a runtime header left the key unchanged"

rm -f "$probe"
[ "$(key)" = "$k1" ] || fail "removing the header did not bring the key back"

echo "  [PASS] cache_key_runtime_headers: runtime header edits change the build cache key"
