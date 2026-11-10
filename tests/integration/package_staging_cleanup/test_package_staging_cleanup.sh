#!/bin/sh
# Regression (#2899): `ae build --emit=lib --package` removes its staging
# directory.
#
# The build synthesizes an entry that imports each module of the package in
# an ae-package-<pid> directory of the temp directory, and nothing removed
# it: every run, built or failed, left one behind (%TEMP% on Windows). The
# directory is now removed when ae exits, whichever way the build ends.
# Each build here gets a temp directory of its own (TMPDIR, and TEMP and TMP
# for Windows), so what is left in it after the build is the build's.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/portable.sh"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] package_staging_cleanup: ae not built"
    exit 0
fi

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) SO_EXT=".dll" ;;
    Darwin) SO_EXT=".dylib" ;;
    *)      SO_EXT=".so" ;;
esac

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail=0

mkdir -p "$WORK/good/src/pk/one" "$WORK/bad/src/pk/one"
cat > "$WORK/good/src/pk/one/module.ae" <<'AEOF'
exports (twice)

twice(n: int) -> int {
    return n * 2
}
AEOF
cat > "$WORK/bad/src/pk/one/module.ae" <<'AEOF'
exports (twice)

twice(n: int) -> int {
    return n * missing_name
}
AEOF

# build NAME: builds the package in $WORK/NAME with a temp directory of its
# own, $WORK/NAME.tmp; the exit status is the build's.
build() {
    mkdir -p "$WORK/$1.tmp"
    t="$WORK/$1.tmp"
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*|Windows_NT) t="$(cygpath -m "$t" 2>/dev/null || echo "$t")" ;;
    esac
    (cd "$WORK/$1" && TMPDIR="$t" TEMP="$t" TMP="$t" AETHER_HOME="$ROOT" \
        "$AE" build --emit=lib --package pk -o "libpk$SO_EXT") >"$WORK/$1.log" 2>&1
}

# left NAME: the ae-package-* entries the build left in its temp directory.
left() {
    ls -d "$WORK/$1.tmp"/ae-package-* 2>/dev/null
}

if build good; then
    if [ ! -f "$WORK/good/libpk$SO_EXT" ]; then
        echo "  [FAIL] success: no libpk$SO_EXT"
        fail=1
    elif [ -n "$(left good)" ]; then
        echo "  [FAIL] success: the staging directory is left: $(left good)"
        fail=1
    else
        echo "  [PASS] success: built, and no staging directory left"
    fi
else
    echo "  [FAIL] success: the package did not build"
    sed 's/^/        /' "$WORK/good.log" | head -10
    fail=1
fi

if build bad; then
    echo "  [FAIL] failure: a package with an error built"
    fail=1
elif ! grep -q "missing_name" "$WORK/bad.log"; then
    echo "  [FAIL] failure: the build failed, but not on the module's error"
    sed 's/^/        /' "$WORK/bad.log" | head -10
    fail=1
elif [ -n "$(left bad)" ]; then
    echo "  [FAIL] failure: the staging directory is left: $(left bad)"
    fail=1
else
    echo "  [PASS] failure: refused, and no staging directory left"
fi

# #2899 review: the staging directory's path was kept in a 1024-byte buffer,
# cut short when longer, and the tree removed at exit was what was left: with
# a temp directory of exactly 1023 bytes, the temp directory itself, and
# everything in it. The path is kept whole now. Linux only: such a path is
# longer than Windows' MAX_PATH and macOS's PATH_MAX allow.
case "$(uname -s)" in
    Linux*)
        long="$WORK/long"
        while [ ${#long} -lt 1023 ]; do
            room=$((1023 - ${#long}))
            seg=$((room - 1))
            [ "$seg" -gt 200 ] && seg=200
            [ $((room - seg - 1)) -eq 1 ] && seg=$((seg - 1))
            long="$long/$(head_c "$seg" /dev/zero | tr '\0' d)"
        done
        if [ ${#long} -ne 1023 ] || ! mkdir -p "$long" 2>/dev/null; then
            echo "  [SKIP] long temp path: cannot make a 1023-byte directory here"
        else
            echo "kept" > "$long/canary"
            (cd "$WORK/good" && TMPDIR="$long" AETHER_HOME="$ROOT" \
                "$AE" build --emit=lib --package pk -o "liblong$SO_EXT") >"$WORK/long.log" 2>&1
            if [ ! -f "$long/canary" ]; then
                echo "  [FAIL] long temp path: the temp directory itself was removed"
                fail=1
            elif ls -d "$long"/ae-package-* >/dev/null 2>&1; then
                echo "  [FAIL] long temp path: the staging directory is left"
                fail=1
            else
                echo "  [PASS] long temp path: only the staging directory is removed"
            fi
        fi
        ;;
    *) echo "  [SKIP] long temp path: Linux only" ;;
esac

exit $fail
