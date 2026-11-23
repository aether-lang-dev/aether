#!/bin/sh
# #3813: a prebuilt libaether.a is linked only when its C runtime is the build
# compiler's. The archive is compiled against one C runtime's headers and
# imports that runtime's symbols: with MinGW GCC's msvcrt archive,
# AE_CC="zig cc" (UCRT) failed to link any program that used try
# ("undefined symbol: __declspec(dllimport) _setjmp"). `make` records the
# archive's runtime in libaether.crt beside it; `ae` asks the compiler
# whether it targets that runtime and, when not, compiles the runtime from
# source with it, as a --trace build does (#1333).
#
#   1. make wrote the record beside the tree's archive.
#   2. A compiler that says it targets another runtime (the real one, given
#      the other runtime's macro on the probes only) builds with the source
#      fallback, -v says so, and the program runs; the compiler as it is
#      links the archive and says nothing.
#   2b. On Windows, an archive recorded for another mingw-w64 major is
#      compiled from source too.
#   3. With zig (on PATH or under build/scratch/.zig), AE_CC="zig cc" builds
#      and runs a program that panics and catches it.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=archive_crt

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] $NAME: ae not built"
    exit 0
fi
if [ ! -f "$ROOT/build/libaether.a" ]; then
    echo "  [SKIP] $NAME: no build/libaether.a (make stdlib)"
    exit 0
fi

WIN=0
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) WIN=1 ;;
esac

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR
unset AETHER_HOME AE_CC CC
fails=0
fail() { echo "  [FAIL] $NAME: $1"; fails=$((fails + 1)); }
done_what=""

# 1. The record.
rec="$ROOT/build/libaether.crt"
if [ ! -f "$rec" ]; then
    fail "make wrote no build/libaether.crt beside the archive"
    crt=""
else
    crt="$(head -n 1 "$rec" | tr -d '\r')"
    case "$crt" in
        *[!a-z0-9_]*) fail "build/libaether.crt holds '$crt', not a C runtime's name" ;;
    esac
    if [ "$WIN" = 1 ] && [ "$crt" != msvcrt ] && [ "$crt" != ucrt ]; then
        fail "on Windows the archive's C runtime is '$crt', not msvcrt or ucrt"
    fi
    done_what="the record says '${crt:-none}'"
fi

printf 'main() {\n    println("crt ok")\n}\n' > "$TMP/hello.ae"

# 2. The other runtime's macro, given on the probes only (their files are
# named ae_crt_*), so the compiler says it targets that runtime while the
# program and the runtime it compiles are built as the compiler builds them.
case "$crt" in
    msvcrt) other_def="-D_UCRT"; other=ucrt ;;
    glibc)  other_def="-D__UCLIBC__"; other=uclibc ;;
    *)      other_def="" ;;
esac
REAL_CC="$(command -v gcc 2>/dev/null || command -v cc 2>/dev/null)"
if [ -n "$other_def" ] && [ -n "$REAL_CC" ]; then
    mkdir -p "$TMP/fake"
    if [ "$WIN" = 1 ]; then
        real="$(cygpath -w "$REAL_CC")"
        printf '@echo off\r\necho %%* | findstr /c:"ae_crt_" >nul\r\nif errorlevel 1 ("%s" %%*) else ("%s" %s %%*)\r\n' \
            "$real" "$real" "$other_def" > "$TMP/fake/gcc.cmd"
        FAKE="$(cygpath -m "$TMP/fake/gcc.cmd")"
    else
        printf '#!/bin/sh\ncase "$*" in *ae_crt_*) exec "%s" %s "$@" ;; esac\nexec "%s" "$@"\n' \
            "$REAL_CC" "$other_def" "$REAL_CC" > "$TMP/fake/gcc"
        chmod +x "$TMP/fake/gcc"
        FAKE="$TMP/fake/gcc"
    fi
    if ! (cd "$TMP" && CC="$FAKE" "$AE" build -v hello.ae -o other) > "$TMP/other.log" 2>&1; then
        sed 's/^/        /' "$TMP/other.log" | grep -i error | head -5
        fail "the build with a compiler on another C runtime failed"
    elif [ "$("$TMP/other" | tr -d '\r')" != "crt ok" ]; then
        fail "the program built from the source fallback did not print 'crt ok'"
    fi
    grep -q "^\[toolchain\] libaether.a is $crt[^;]*; .* targets $other[^:]*: the runtime is compiled from source" "$TMP/other.log" ||
        fail "-v does not say the runtime was compiled from source for a compiler on $other: $(grep '^\[toolchain\]' "$TMP/other.log" | tail -1 | cut -c1-160)"
    # The compiler as it is: the archive, and no such line.
    if ! (cd "$TMP" && "$AE" build -v hello.ae -o same) > "$TMP/same.log" 2>&1; then
        fail "the build with the archive's own compiler failed"
    elif grep -q "the runtime is compiled from source" "$TMP/same.log"; then
        fail "the archive's own compiler took the source fallback"
    fi
    done_what="$done_what; a compiler on $other compiles the runtime from source and says so, the archive's own links it"
fi

# 2b. On Windows the record's second line is the archive's mingw-w64 major,
# and a compiler on another mingw-w64 takes the source fallback too: the
# same C runtime by name, other import libraries (WinLibs GCC 14.2's
# mingw-w64 12 has no _strtof_l import, which MSYS2's 15 archive takes). A
# toolchain of its own here, whose record names a mingw-w64 no compiler has.
mingw="$(sed -n 's/^mingw-w64 \([0-9][0-9]*\)$/\1/p' "$rec" 2>/dev/null | tr -d '\r' | head -n 1)"
if [ -n "$mingw" ] && [ -n "$crt" ]; then
    PRIV="$TMP/tc"
    mkdir -p "$PRIV/build"
    for d in runtime std include; do ln -s "$ROOT/$d" "$PRIV/$d"; done
    for f in ae ae.exe aetherc aetherc.exe libaether.a libaether_main.a MANIFEST aether_manifest.o; do
        [ -f "$ROOT/build/$f" ] && cp "$ROOT/build/$f" "$PRIV/build/"
    done
    printf '%s\nmingw-w64 99\n' "$crt" > "$PRIV/build/libaether.crt"
    if ! (cd "$TMP" && "$PRIV/build/ae" build -v hello.ae -o mingw) > "$TMP/mingw.log" 2>&1; then
        grep -i "error" "$TMP/mingw.log" | head -3 | cut -c1-200 | sed 's/^/        /'
        fail "the build against an archive of another mingw-w64 failed"
    elif [ "$("$TMP/mingw" | tr -d '\r')" != "crt ok" ]; then
        fail "the program built for another mingw-w64 did not print 'crt ok'"
    fi
    grep -q "^\[toolchain\] libaether.a is $crt mingw-w64 99; .* targets $crt mingw-w64 $mingw: the runtime is compiled from source" "$TMP/mingw.log" ||
        fail "-v does not say the runtime was compiled from source for mingw-w64 $mingw against an archive of 99: $(grep '^\[toolchain\]' "$TMP/mingw.log" | tail -1 | cut -c1-160)"
    done_what="$done_what; a compiler on another mingw-w64 compiles it from source too"
fi

# 3. zig.
. "$ROOT/tests/lib/find_zig.sh"
zig="$(find_zig "$ROOT")"
if [ -n "$zig" ]; then
    printf 'main() {\n    try {\n        panic("boom")\n    } catch e {\n        println("caught ${e}")\n    }\n}\n' > "$TMP/catch.ae"
    if ! (cd "$TMP" && AE_CC="$zig cc" "$AE" build catch.ae -o catch) > "$TMP/zig.log" 2>&1; then
        grep -i "error" "$TMP/zig.log" | head -3 | cut -c1-200 | sed 's/^/        /'
        fail "AE_CC=\"zig cc\" did not build a program that panics and catches it"
    elif [ "$("$TMP/catch" | tr -d '\r')" != "caught boom" ]; then
        fail "the program built with AE_CC=\"zig cc\" did not print 'caught boom'"
    fi
    done_what="$done_what; AE_CC=\"zig cc\" builds and runs a panic and its catch"
fi

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: $done_what"
exit 0
