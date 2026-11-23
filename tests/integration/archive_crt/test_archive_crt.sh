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
# What this test checks is mostly the decision, which `ae build -v` states
# before anything is compiled. Steps 2 and 2b end in the source fallback
# and build with a stand-in compiler that answers the C runtime probes (the
# files named ae_crt_*) with the real one and fails every other compile at
# once, so they stop right after the decision instead of compiling the
# whole runtime. One build goes through the fallback end to end, compiling
# the runtime and running the program: step 2c on Windows (msvcrt archives,
# -mcrtdll=ucrt, --quick; about 9 s on a 24-thread Windows box), and step 3
# wherever zig is found. On a platform with neither (Linux with no zig) the
# fallback build itself is runtime_cache's (--trace) to check.
#
#   1. make wrote the record beside the tree's archive, and the record is a
#      product of the archive: relinking ae does not rewrite it. It names
#      the compiler as it was when the archive was built, so a compiler
#      upgraded since (another mingw-w64 with it) is asked, not trusted.
#   2. A compiler that says it targets another runtime (the real one, given
#      the other runtime's macro on the probes only) takes the source
#      fallback, -v says so, and the program's key folds the runtime's; the
#      compiler as it is links the archive and the program runs.
#   2b. On Windows, an archive recorded for another mingw-w64 major takes
#      the source fallback too.
#   2c. cflags that change the C runtime (-mcrtdll=ucrt) are asked about,
#      and the program, a try and catch on the runtime compiled from source
#      for UCRT, builds and runs.
#   2d. The record names the compiler that built the archive (`cc <id>`, as
#      `ae cache ccid` prints it), and a build by that compiler with no
#      cflags, on a cold cache, links the archive without asking: no
#      ae_crt_probe compile. Asking was one more compiler start on every
#      cold cache (about 25 ms on Windows).
#   2e. A warning flag of the user's (-Werror -Wmissing-variable-declarations)
#      does not make the archive's own compiler "target another C runtime":
#      the probe's control is compiled with the same cflags as the probe,
#      and the probe declares what it defines. The build links the archive,
#      so its key does not fold the runtime's sources.
#   2f. A compiler that cannot be asked (its probes fail, controls too)
#      links the archive, and the decision is remembered: a warm build does
#      not ask again from its cache key.
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

# 1b. Relinking ae leaves the record as it is: make, told ae is new (-W,
# which changes no file), does not run the record's recipe, whose last line
# says "C runtime of libaether.a". First made up to date as it is (the
# flags stamp's recipe runs every time and changes nothing when the flags
# are the same).
exe=""
[ -f "$ROOT/build/ae.exe" ] && exe=".exe"
MAKE_CMD="$(command -v gmake 2>/dev/null || command -v make 2>/dev/null)"
if [ -n "$MAKE_CMD" ] && [ -f "$rec" ] &&
   (cd "$ROOT" && "$MAKE_CMD" -s build/libaether.crt) > "$TMP/make0.log" 2>&1; then
    (cd "$ROOT" && "$MAKE_CMD" -s -W "build/ae$exe" build/libaether.crt) > "$TMP/make1.log" 2>&1
    if grep -q "C runtime of libaether.a" "$TMP/make1.log"; then
        fail "a relink of ae rewrites build/libaether.crt, the record of an archive that is not rebuilt"
    else
        done_what="$done_what; relinking ae leaves it as it is"
    fi
else
    echo "  [SKIP] $NAME: step 1b: no make, or build/libaether.crt could not be brought up to date"
fi

printf 'main() {\n    println("crt ok")\n}\n' > "$TMP/hello.ae"
REAL_CC="$(command -v gcc 2>/dev/null || command -v cc 2>/dev/null)"

# stand_in <name> <defines>: a compiler that compiles the C runtime probes
# with the real one and <defines>, and fails anything else at once. Prints
# the path to give as CC.
stand_in() {
    mkdir -p "$TMP/$1"
    if [ "$WIN" = 1 ]; then
        real="$(cygpath -w "$REAL_CC")"
        printf '@echo off\r\necho %%* | findstr /c:"ae_crt_" >nul\r\nif errorlevel 1 exit /b 1\r\n"%s" %s %%*\r\n' \
            "$real" "$2" > "$TMP/$1/gcc.cmd"
        cygpath -m "$TMP/$1/gcc.cmd"
    else
        printf '#!/bin/sh\ncase "$*" in *ae_crt_*) exec "%s" %s "$@" ;; esac\nexit 1\n' \
            "$REAL_CC" "$2" > "$TMP/$1/gcc"
        chmod +x "$TMP/$1/gcc"
        printf '%s\n' "$TMP/$1/gcc"
    fi
}
# decided <log>: the [toolchain] line about the archive, for a message.
decided() { grep '^\[toolchain\] libaether.a' "$TMP/$1" | tail -1 | cut -c1-160; }
# folds <log>: whether the program's key folded the runtime's sources.
folds() { tr -d '\r' < "$TMP/$1" | grep '^\[cache\] key parts: ' | tail -1 | grep -q ':rt='; }

# 2. The other runtime's macro, given on the probes only, so the compiler
# says it targets that runtime.
case "$crt" in
    msvcrt) other_def="-D_UCRT"; other=ucrt ;;
    glibc)  other_def="-D__UCLIBC__"; other=uclibc ;;
    *)      other_def="" ;;
esac
if [ -n "$other_def" ] && [ -n "$REAL_CC" ]; then
    FAKE="$(stand_in other "$other_def")"
    (cd "$TMP" && CC="$FAKE" "$AE" build -v hello.ae -o other) > "$TMP/other.log" 2>&1
    grep -q "^\[toolchain\] libaether.a is $crt[^;]*; .* targets $other[^:]*: the runtime is compiled from source" "$TMP/other.log" ||
        fail "-v does not say the runtime was compiled from source for a compiler on $other: $(decided other.log)"
    folds other.log ||
        fail "the key of a build that compiles the runtime from source does not fold the runtime's: $(grep '^\[cache\] key parts' "$TMP/other.log" | tail -1 | cut -c1-160)"
    # The compiler as it is: the archive is linked, and the program runs.
    if ! (cd "$TMP" && "$AE" build -v hello.ae -o same) > "$TMP/same.log" 2>&1; then
        fail "the build with the archive's own compiler failed"
    elif [ "$("$TMP/same" | tr -d '\r')" != "crt ok" ]; then
        fail "the program the archive's own compiler built did not print 'crt ok'"
    elif grep -q "the runtime is compiled from source" "$TMP/same.log"; then
        fail "the archive's own compiler took the source fallback"
    elif ! grep -Eq "^\[toolchain\] libaether.a is $crt[^;]*; .* (targets the same|built it): the archive is linked" "$TMP/same.log"; then
        fail "-v does not say the archive's own compiler targets its C runtime: $(decided same.log)"
    fi
    done_what="$done_what; a compiler on $other takes the source fallback and says so, the archive's own links it"
fi

# 2b. On Windows the record's second line is the archive's mingw-w64 major,
# and a compiler on another mingw-w64 takes the source fallback too: the
# same C runtime by name, other import libraries (WinLibs GCC 14.2's
# mingw-w64 12 has no _strtof_l import, which MSYS2's 15 archive took until
# #3818). A toolchain of its own here, whose record names a mingw-w64 no
# compiler has.
mingw="$(sed -n 's/^mingw-w64 \([0-9][0-9]*\)$/\1/p' "$rec" 2>/dev/null | tr -d '\r' | head -n 1)"
if [ -n "$mingw" ] && [ -n "$crt" ] && [ -n "$REAL_CC" ]; then
    PRIV="$TMP/tc"
    mkdir -p "$PRIV/build"
    for d in runtime std include; do ln -s "$ROOT/$d" "$PRIV/$d"; done
    for f in ae ae.exe aetherc aetherc.exe libaether.a libaether_main.a MANIFEST aether_manifest.o; do
        [ -f "$ROOT/build/$f" ] && cp "$ROOT/build/$f" "$PRIV/build/"
    done
    printf '%s\nmingw-w64 99\n' "$crt" > "$PRIV/build/libaether.crt"
    ASIS="$(stand_in asis "")"
    (cd "$TMP" && CC="$ASIS" "$PRIV/build/ae" build -v hello.ae -o mingw) > "$TMP/mingw.log" 2>&1
    grep -q "^\[toolchain\] libaether.a is $crt mingw-w64 99; .* targets $crt mingw-w64 $mingw: the runtime is compiled from source" "$TMP/mingw.log" ||
        fail "-v does not say the runtime was compiled from source for mingw-w64 $mingw against an archive of 99: $(decided mingw.log)"
    done_what="$done_what; a compiler on another mingw-w64 takes it too"
fi

# 2c. The project's own cflags can change the C runtime: with
# `-mcrtdll=ucrt`, MinGW GCC builds against UCRT. The question is asked with
# them, so an msvcrt archive is not linked into a UCRT program.
if [ "$crt" = msvcrt ]; then
    mkdir -p "$TMP/mcrt"
    printf 'main() {\n    try {\n        panic("boom")\n    } catch e {\n        println("caught ${e}")\n    }\n}\n' > "$TMP/mcrt/catch.ae"
    printf '[build]\ncflags = "-mcrtdll=ucrt"\n' > "$TMP/mcrt/aether.toml"
    if ! (cd "$TMP/mcrt" && "$AE" build -v --quick catch.ae -o mcrt) > "$TMP/mcrt.log" 2>&1; then
        grep -i "error" "$TMP/mcrt.log" | head -3 | cut -c1-200 | sed 's/^/        /'
        fail "the build with cflags -mcrtdll=ucrt, on the runtime compiled from source, failed"
    elif [ "$("$TMP/mcrt/mcrt" | tr -d '\r')" != "caught boom" ]; then
        fail "the program built with -mcrtdll=ucrt did not print 'caught boom'"
    fi
    grep -q "^\[toolchain\] libaether.a is msvcrt[^;]*; .* targets ucrt[^:]*: the runtime is compiled from source" "$TMP/mcrt.log" ||
        fail "with cflags -mcrtdll=ucrt, -v does not say the runtime was compiled from source for ucrt: $(decided mcrt.log)"
    done_what="$done_what; cflags naming another C runtime (-mcrtdll=ucrt) are asked about, and a try and catch builds and runs on the runtime compiled from source"
fi

# 2d. The compiler that built the archive, in the record, and not asked.
built_by="$(sed -n 's/^cc \([0-9a-f]\{16\}\)$/\1/p' "$rec" 2>/dev/null | tr -d '\r' | head -n 1)"
if [ -n "$crt" ]; then
    mine="$("$AE" cache ccid 2>/dev/null | tr -d '\r')"
    if [ -z "$built_by" ]; then
        # make leaves the line out when the compiler with no flags names
        # another runtime than with the build's CFLAGS, or ae could not run.
        echo "  [SKIP] $NAME: step 2d: build/libaether.crt names no compiler (no 'cc <id>' line)"
    elif [ "$mine" != "$built_by" ]; then
        echo "  [SKIP] $NAME: step 2d: the C compiler here ($mine) is not the one that built the archive ($built_by)"
    elif ! (cd "$TMP" && AETHER_CACHE_DIR="$TMP/cache_own" "$AE" build -v hello.ae -o own) > "$TMP/own.log" 2>&1; then
        grep -i "error" "$TMP/own.log" | head -3 | cut -c1-200 | sed 's/^/        /'
        fail "the cold build with the archive's own compiler failed"
    elif grep -q "ae_crt_probe" "$TMP/own.log"; then
        fail "on a cold cache the archive's own compiler was asked which C runtime it targets: $(grep ae_crt_probe "$TMP/own.log" | head -1 | cut -c1-160)"
    elif ! grep -q "^\[toolchain\] libaether.a is $crt[^;]*; .* built it: the archive is linked" "$TMP/own.log"; then
        fail "-v does not say the archive's own compiler built it: $(decided own.log)"
    else
        done_what="$done_what; the archive's own compiler is not asked on a cold cache"
    fi
fi

# 2e. A warning flag of the user's, on the archive's own compiler: asked
# (cflags), and the answer is the archive's runtime. Only where the compiler
# takes the flag (GCC 14 and later, Clang).
if [ -n "$crt" ] && [ -n "$REAL_CC" ]; then
    mkdir -p "$TMP/warn"
    printf 'extern int ae_w;\nint ae_w;\n' > "$TMP/warn/w.c"
    if "$REAL_CC" -Werror -Wmissing-variable-declarations -fsyntax-only "$TMP/warn/w.c" 2>/dev/null; then
        cp "$TMP/hello.ae" "$TMP/warn/"
        printf '[build]\ncflags = "-Werror -Wmissing-variable-declarations"\n' > "$TMP/warn/aether.toml"
        # The build itself may stop on a warning in the generated C; the
        # question here is what ae decided about the archive.
        (cd "$TMP/warn" && AETHER_CACHE_DIR="$TMP/cache_warn" "$AE" build -v hello.ae -o warn) > "$TMP/warn.log" 2>&1
        if grep -q "the runtime is compiled from source" "$TMP/warn.log"; then
            fail "cflags -Werror -Wmissing-variable-declarations made the archive's own compiler 'target another C runtime': $(decided warn.log)"
        elif ! grep -q "^\[toolchain\] libaether.a is $crt[^;]*; .* targets the same: the archive is linked" "$TMP/warn.log"; then
            fail "with cflags -Werror -Wmissing-variable-declarations, -v does not say the compiler targets the archive's runtime: $(decided warn.log)"
        elif folds warn.log; then
            fail "a build with cflags that links the archive folded the runtime's sources into its key"
        else
            done_what="$done_what; a warning flag of the user's is no other C runtime"
        fi
    else
        echo "  [SKIP] $NAME: step 2e: $REAL_CC does not take -Wmissing-variable-declarations"
    fi
fi

# 2f. A compiler that fails the C runtime probes, and their controls, and
# compiles anything else as the real one does: it cannot be asked, so the
# archive is linked; the next build, a cache hit, does not ask again.
if [ -n "$crt" ] && [ -n "$REAL_CC" ]; then
    mkdir -p "$TMP/mute"
    if [ "$WIN" = 1 ]; then
        printf '@echo off\r\necho %%* | findstr /c:"ae_crt_" >nul\r\nif not errorlevel 1 exit /b 1\r\n"%s" %%*\r\n' \
            "$(cygpath -w "$REAL_CC")" > "$TMP/mute/gcc.cmd"
        MUTE="$(cygpath -m "$TMP/mute/gcc.cmd")"
    else
        printf '#!/bin/sh\ncase "$*" in *ae_crt_*) exit 1 ;; esac\nexec "%s" "$@"\n' "$REAL_CC" > "$TMP/mute/gcc"
        chmod +x "$TMP/mute/gcc"
        MUTE="$TMP/mute/gcc"
    fi
    if ! (cd "$TMP" && CC="$MUTE" AETHER_CACHE_DIR="$TMP/cache_mute" "$AE" build -v hello.ae -o mute) > "$TMP/mute1.log" 2>&1; then
        grep -i "error" "$TMP/mute1.log" | head -3 | cut -c1-200 | sed 's/^/        /'
        fail "the build with a compiler that cannot be asked failed"
    elif ! grep -q "could not be asked which C runtime it targets: the archive is linked" "$TMP/mute1.log"; then
        fail "-v does not say the compiler could not be asked: $(decided mute1.log)"
    elif ! (cd "$TMP" && CC="$MUTE" AETHER_CACHE_DIR="$TMP/cache_mute" "$AE" build -v hello.ae -o mute) > "$TMP/mute2.log" 2>&1; then
        fail "the second build with a compiler that cannot be asked failed"
    elif ! grep -q '^\[cache\] hit: ' "$TMP/mute2.log"; then
        fail "the second build with a compiler that cannot be asked was not a cache hit"
    elif grep -q "ae_crt_probe" "$TMP/mute2.log"; then
        fail "a cache hit asked again which C runtime a compiler that cannot be asked targets: $(grep -c ae_crt_probe "$TMP/mute2.log") probe compiles"
    else
        done_what="$done_what; a compiler that cannot be asked links the archive and is not asked again"
    fi
fi

# 3. zig.
. "$ROOT/tests/lib/find_zig.sh"
zig="$(find_zig "$ROOT")"
if [ -n "$zig" ]; then
    printf 'main() {\n    try {\n        panic("boom")\n    } catch e {\n        println("caught ${e}")\n    }\n}\n' > "$TMP/catch.ae"
    if ! (cd "$TMP" && AE_CC="$zig cc" "$AE" build -v catch.ae -o catch) > "$TMP/zig.log" 2>&1; then
        grep -i "error" "$TMP/zig.log" | head -3 | cut -c1-200 | sed 's/^/        /'
        fail "AE_CC=\"zig cc\" did not build a program that panics and catches it"
    elif [ "$("$TMP/catch" | tr -d '\r')" != "caught boom" ]; then
        fail "the program built with AE_CC=\"zig cc\" did not print 'caught boom'"
    fi
    if grep -q "the runtime is compiled from source" "$TMP/zig.log"; then
        done_what="$done_what; AE_CC=\"zig cc\" builds and runs a panic and its catch"
    else
        echo "  [SKIP] $NAME: step 3 (zig): zig targets the archive's C runtime here ('${crt:-none}'), so the build linked the archive and the source fallback was not exercised"
    fi
else
    echo "  [SKIP] $NAME: step 3 (zig): no zig on PATH or under build/scratch/.zig"
fi

# 4. The msvcrt WinLibs, when one is installed (tests/lib/find_msvcrt_gcc.sh).
# Its mingw-w64 (12) differs from the archive's, so the runtime is compiled
# from source with it, and its import library has no _strtof_l, which the
# runtime called on msvcrt: that build did not link (#3818). A program that
# parses floats next to float ties, in the C locale and under a comma-decimal
# one, builds with it and gets UCRT strtof's bits.
. "$ROOT/tests/lib/find_msvcrt_gcc.sh"
mgcc=""
[ "$WIN" = 1 ] && mgcc="$(find_msvcrt_gcc "$ROOT")"
if [ -n "$mgcc" ]; then
    halfway="$ROOT/tests/regression/test_string_parse_float_halfway.ae"
    if ! (cd "$TMP" && AE_CC="$(cygpath -m "$mgcc")" "$AE" build -v "$halfway" -o halfway) > "$TMP/msvcrt.log" 2>&1; then
        grep -o "undefined reference to .*\|error: .*" "$TMP/msvcrt.log" | head -3 | cut -c1-200 | sed 's/^/        /'
        fail "AE_CC=$mgcc did not build a program that parses floats"
    elif ! PATH="$(dirname "$mgcc"):$PATH" "$TMP/halfway" > "$TMP/halfway.out" 2>&1 ||
         ! grep -q "^All PASS" "$TMP/halfway.out"; then
        grep "FAIL" "$TMP/halfway.out" | head -5 | cut -c1-240 | sed 's/^/        /'
        fail "the float parsing program built with AE_CC=$mgcc did not pass"
    else
        done_what="$done_what; the msvcrt WinLibs builds a program that parses floats, with UCRT's bits"
    fi
elif [ "$WIN" = 1 ]; then
    echo "  [SKIP] $NAME: step 4 (msvcrt WinLibs): none in AETHER_TEST_MSVCRT_GCC or under build/scratch/.winlibs-msvcrt"
fi

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: $done_what"
exit 0
