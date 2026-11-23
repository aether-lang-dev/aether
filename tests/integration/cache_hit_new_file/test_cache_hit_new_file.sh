#!/bin/sh
# Regression (#3801): a cache hit writes the program as a new file, never
# over the old one in place.
#
# On a hit `ae build` copied the cached binary to the output path by opening
# the existing file for writing, so a program that had run kept its inode
# with new bytes, and macOS killed the next run of it (SIGKILL, exit 137)
# though its bytes and its signature were fine: under load 34 runs of 40 in a
# shell model of the same pattern. The copy now goes to a temporary beside
# the output, renamed over it. Checked here by the inode, which is the
# mechanism and does not depend on the machine's load: after a hit the
# output is another file, it runs, and no temporary is left beside it.
# Each build in the loop runs the program it got, as the failing sweep did.
# On Windows `ls -i` gives the NTFS file id, which a rename over the file
# changes and a rewrite in place keeps, so the check holds there too (it
# failed there on every hit before the change).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] cache_hit_new_file: ae not built"
    exit 0
fi

WIN=0
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) WIN=1 ;;
esac
EXE=""
[ "$WIN" = 1 ] && EXE=".exe"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Hits are asserted, so the cache must be this test's alone.
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR

mkdir -p "$TMP/proj"
cd "$TMP/proj" || exit 1
cat > main.ae <<'AEOF'
main() {
    println("v=7")
}
AEOF

fail=0
inode() {
    ls -i "$1" 2>/dev/null | awk '{print $1}'
}

build_and_run() {
    if ! "$AE" build main.ae -o ./app > "$TMP/build.log" 2>&1; then
        echo "  [FAIL] cache_hit_new_file: build $1 failed"
        sed 's/^/        /' "$TMP/build.log" | head -10
        exit 1
    fi
    got=$("./app$EXE" 2>&1 | tr -d '\r')
    if [ "$got" != "v=7" ]; then
        echo "  [FAIL] cache_hit_new_file: build $1 ran as '$got', expected 'v=7'"
        fail=1
    fi
}

build_and_run 1
hits=0
for i in 2 3 4 5 6; do
    before=$(inode "./app$EXE")
    build_and_run "$i"
    grep -q "cache hit" "$TMP/build.log" && hits=$((hits + 1))
    after=$(inode "./app$EXE")
    if [ -n "$before" ] && [ "$before" = "$after" ]; then
        echo "  [FAIL] cache_hit_new_file: build $i rewrote ./app in place (inode $before kept)"
        fail=1
    fi
done
if [ "$hits" -lt 5 ]; then
    echo "  [FAIL] cache_hit_new_file: $hits of 5 later builds were cache hits, expected 5"
    fail=1
fi
left=$(ls -a | grep -c '\.ae-tmp-')
if [ "$left" != "0" ]; then
    echo "  [FAIL] cache_hit_new_file: $left temporary file(s) left beside the program"
    ls -a | sed 's/^/        /'
    fail=1
fi

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "  [PASS] cache_hit_new_file: a cache hit writes the program as a new file and leaves nothing behind"
exit 0
