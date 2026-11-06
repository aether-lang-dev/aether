#!/bin/sh
# Regression (#3599): a first build looks nothing up in the cache.
#
# A source with no depfile yet would be looked up under a key made by
# walking its directory and the working directory, file by file. No build
# publishes under that key: once aetherc has written the depfile, the build
# publishes under the key the depfile gives (#2500). So the lookup could not
# hit, and its walk cost every first build (every build of a CI sweep) about
# 170 ms on Windows. The first `ae build` and `ae run` of a source now skip
# it and still publish, and the second of each hits.
#
# A build that had to publish under a tree-walk key (no depfile it could
# fold) leaves a marker in the cache directory, `walk-keyed`. With it there,
# a first build looks up under the walk key as before, so such an entry is
# still found.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] cache_cold_no_walk: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR
unset AETHER_HOME
fails=0
fail() { echo "  [FAIL] cache_cold_no_walk: $1"; fails=$((fails + 1)); }

mkdir -p "$TMP/proj"
cd "$TMP/proj" || exit 1
printf 'main() {\n    println("cold build")\n}\n' > a.ae
printf 'main() {\n    println("cold run")\n}\n' > b.ae
printf 'main() {\n    println("walked")\n}\n' > c.ae

# The cache lines a verbose build or run prints.
lines() { grep '^\[cache\]' "$1" | tr '\n' '|'; }

"$AE" build -v a.ae -o a > build1.log 2>&1 || { tail -5 build1.log; fail "the first build failed"; }
case "$(lines build1.log)" in
    *"miss: no depfile yet"*"wrote:"*) ;;
    *) fail "the first build did not skip the lookup and publish: $(lines build1.log)" ;;
esac
"$AE" build -v a.ae -o a > build2.log 2>&1 || fail "the second build failed"
case "$(lines build2.log)" in
    *"hit:"*) ;;
    *) fail "the second build did not hit: $(lines build2.log)" ;;
esac

"$AE" run -v b.ae > run1.log 2>&1 || { tail -5 run1.log; fail "the first run failed"; }
grep -q '^cold run$' run1.log || fail "the first run printed the wrong thing"
case "$(lines run1.log)" in
    *"miss: no depfile yet"*) ;;
    *) fail "the first run did not skip the lookup: $(lines run1.log)" ;;
esac
"$AE" run -v b.ae > run2.log 2>&1 || fail "the second run failed"
case "$(lines run2.log)" in
    *"hit:"*) ;;
    *) fail "the second run did not hit: $(lines run2.log)" ;;
esac

[ -e "$AETHER_CACHE_DIR/walk-keyed" ] && fail "a build that folded its depfile left the walk-keyed marker"

# With the marker, a first build makes the walk key and looks it up.
: > "$AETHER_CACHE_DIR/walk-keyed"
"$AE" build -v c.ae -o c > build3.log 2>&1 || fail "the build with the marker failed"
case "$(lines build3.log)" in
    *"no depfile yet"*) fail "with the marker, the first build still skipped the lookup: $(lines build3.log)" ;;
    *"miss: "[0-9a-f]*) ;;
    *) fail "with the marker, the first build made no key: $(lines build3.log)" ;;
esac

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] cache_cold_no_walk: a first build or run skips the lookup and publishes; the second hits; the marker brings the walk back"
