#!/bin/sh
# #3841: a program's compile line names only the -idirafter directories its
# C needs: the ones its includes resolve in, and the includes of those, in
# turn. The toolchain's list (ae cflags) names every runtime and std
# directory with a header, 53 in a source tree, where hello needs 3.
#
#   1. hello: fewer -idirafter directories than `ae cflags` names, more than
#      none; it builds and runs.
#   2. transitive/: a @c_include header beside the program includes a std
#      header by its bare name, found only through the list: the scan
#      follows it, and the program builds and prints `magic 1`.
#   3. macro/: the same include through a macro, which the scan cannot
#      follow: the line names every directory (the fallback), and the
#      program builds and prints `magic 1`.
# The #2552 order holds: every one is -idirafter, after the system's.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -f "$AE.exe" ] && AE="$AE.exe"
NAME=idirafter_program_list

if [ ! -f "$AE" ]; then
    echo "  [SKIP] $NAME: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR
unset AETHER_HOME AE_CC
fails=0
fail() { echo "  [FAIL] $NAME: $1"; fails=$((fails + 1)); }

cp -r "$SCRIPT_DIR/hello.ae" "$SCRIPT_DIR/transitive" "$SCRIPT_DIR/macro" "$TMP/"
cd "$TMP" || exit 1

full=$("$AE" cflags 2>/dev/null | tr ' ' '\n' | grep -c -x -- '-idirafter')
[ "$full" -gt 3 ] || { echo "  [SKIP] $NAME: ae cflags names $full -idirafter directories"; exit 0; }

# dirs <log>: the -idirafter count of the compile line in an `ae build -v` log.
dirs() {
    grep '^\[cmd\] .* -o ' "$1" | grep -v -- ' -c ' | grep -v 'ae_avx_probe\|--version' | tail -1 |
        tr ' ' '\n' | grep -c -x -- '-idirafter'
}

if ! "$AE" build -v hello.ae -o hello > hello.log 2>&1; then
    grep -i -m3 "error" hello.log | cut -c1-200 | sed 's/^/        /'
    fail "hello did not build"
else
    n=$(dirs hello.log)
    { [ "$n" -gt 0 ] && [ "$n" -lt "$full" ]; } ||
        fail "hello's compile line names $n -idirafter directories, of the $full ae cflags names"
    [ "$(./hello | tr -d '\r')" = "hello" ] || fail "hello printed something else"
fi

for p in transitive macro; do
    if ! (cd "$p" && "$AE" build -v main.ae -o main) > "$p.log" 2>&1; then
        grep -i -m3 "error" "$p.log" | cut -c1-200 | sed 's/^/        /'
        fail "$p/main.ae did not build"
        continue
    fi
    out="$("./$p/main" | tr -d '\r')"
    [ "$out" = "magic 1" ] || fail "$p/main.ae printed '$out', not 'magic 1'"
    n=$(dirs "$p.log")
    if [ "$p" = macro ]; then
        [ "$n" -eq "$full" ] ||
            fail "macro/: an include through a macro, yet the line names $n of $full directories"
    else
        [ "$n" -lt "$full" ] ||
            fail "transitive/: the line names every directory ($n), not the ones the scan found"
    fi
done

if [ "$fails" -ne 0 ]; then
    exit 1
fi
echo "  [PASS] $NAME: a compile names the -idirafter directories its includes reach, every one when they cannot be followed"
exit 0
