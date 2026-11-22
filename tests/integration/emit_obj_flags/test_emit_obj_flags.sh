#!/bin/sh
# #3812: a native `ae build --emit=obj` compiled the generated C with none of
# the flags every other build uses: no -fwrapv, no optimisation level, no
# user cflags. So signed overflow was undefined in the object and defined in
# the executable, and a project whose cflags the program needs could not
# build an object at all. The object's compile line now comes from the same
# flags as the executable's compile step.
#
# obj.ae reads AE_OBJ_TAG, which only aether.toml's cflags define, and
# computes a seed that wraps past 2^31. Its executable, and its object linked
# into a program by the C compiler with `ae cflags --libs`, must print the
# same line; the object's compile line must carry -fwrapv, the -O level of
# the build and the user's cflags.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=emit_obj_flags

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] $NAME: ae not built"
    exit 0
fi
CC="${CC:-cc}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR
unset AETHER_HOME AE_CC
fails=0
fail() { echo "  [FAIL] $NAME: $1"; fails=$((fails + 1)); }

cp "$SCRIPT_DIR/obj.ae" "$SCRIPT_DIR/aether.toml" "$TMP/"
cd "$TMP" || exit 1

if ! "$AE" build obj.ae -o exe > exe.log 2>&1; then
    sed 's/^/        /' exe.log | head -5
    fail "the executable did not build"
fi
want="$(./exe 2>&1 | tr -d '\r')"
case "$want" in
    "tag 7 seed "*) ;;
    *) fail "the executable printed '$want', not 'tag 7 seed ...'" ;;
esac

if ! "$AE" build -v --emit=obj obj.ae -o obj.o > obj.log 2>&1; then
    grep -i "error" obj.log | head -3 | cut -c1-200 | sed 's/^/        /'
    fail "the --emit=obj build failed"
else
    line="$(grep '^\[cmd\].* -c .*obj\.o' obj.log | tail -1)"
    for flag in -fwrapv -O2 -DAE_OBJ_TAG=7; do
        printf '%s\n' "$line" | tr ' ' '\n' | grep -qx -- "$flag" ||
            fail "the object's compile line has no $flag: $(printf '%s' "$line" | cut -c1-200)"
    done
    LIBS="$("$AE" cflags --libs 2>/dev/null)"
    if ! $CC obj.o $LIBS -o objprog > link.log 2>&1; then
        sed 's/^/        /' link.log | head -5
        fail "the object did not link into a program"
    else
        got="$(./objprog 2>&1 | tr -d '\r')"
        [ "$got" = "$want" ] ||
            fail "the object's program printed '$got', the executable '$want'"
    fi
fi

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: the --emit=obj object is compiled as the executable is (-fwrapv, -O2, the user's cflags) and prints what it prints"
exit 0
