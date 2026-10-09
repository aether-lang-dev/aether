#!/bin/sh
# Regression (#2533): on Windows, a C compiler that resolves to a batch file
# (a gcc.cmd wrapper or shim on PATH, or $CC naming one) runs through
# cmd.exe, whose command line is capped at 8191 characters, so `ae build`
# with a long command line (a deep directory, many --extra files) failed
# with "The command line is too long". ae now hands such a compiler its
# arguments through a response file (`gcc @file`), which the real compiler
# reads; an executable compiler keeps the direct spawn.
#
# The compiler here is a .cmd wrapper around the real gcc that adds a -D the
# program prints, so the build proves the wrapper ran AND the arguments
# arrived whole; the command line is pushed past the cap with --extra files
# under a long directory. Windows only: elsewhere a compiler is never a
# batch file.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) ;;
    *) echo "  [SKIP] win_batch_compiler_long_cmdline: Windows only"; exit 0 ;;
esac
if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] win_batch_compiler_long_cmdline: ae not built"
    exit 0
fi
REAL_CC="$(command -v gcc 2>/dev/null)"
if [ -z "$REAL_CC" ]; then
    echo "  [SKIP] win_batch_compiler_long_cmdline: no gcc on PATH"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR
unset AE_CC CC

mkdir -p "$TMP/cc"
printf '@"%s" -DAE_CC_TAG=7 %%*\r\n' "$(cygpath -w "$REAL_CC")" > "$TMP/cc/gcc.cmd"

# A directory whose name alone is long, so every --extra path is too.
DEEP="$TMP/$(printf 'd%.0s' $(seq 1 60))/$(printf 'e%.0s' $(seq 1 60))/$(printf 'f%.0s' $(seq 1 60))"
mkdir -p "$DEEP"
cd "$DEEP" || exit 1

cat > main.ae <<'AEOF'
extern cc_tag() -> int
extern part_sum() -> int

main() {
    println("tag=${cc_tag()} parts=${part_sum()}")
}
AEOF

cat > tag.c <<'COF'
int cc_tag(void) {
#ifdef AE_CC_TAG
    return AE_CC_TAG;
#else
    return 0;
#endif
}
COF

# Extra files (one `--extra` each), each named by an absolute path of about
# 250 characters: with the deep directory in every other path of the C
# command line, this takes it well past 8191 characters, while the --extra
# list itself stays under ae's own 8 KiB cap on it.
extras="--extra tag.c"
i=0
while [ $i -lt 25 ]; do
    f="part_with_a_long_file_name_to_lengthen_the_command_line_$i.c"
    printf 'int part_%d(void) { return 1; }\n' "$i" > "$f"
    extras="$extras --extra $(cygpath -m "$DEEP/$f")"
    i=$((i + 1))
done
{
    printf 'int part_sum(void) {\n    int s = 0;\n'
    i=0
    while [ $i -lt 25 ]; do printf '    { extern int part_%d(void); s += part_%d(); }\n' "$i" "$i"; i=$((i + 1)); done
    printf '    return s;\n}\n'
} > sum.c
extras="$extras --extra sum.c"

# $extras is expanded unquoted below, to split it into words, and nothing in
# it is meant as a pattern. Pathname expansion stays off: these paths are
# past 260 characters, so cygpath writes them in the long-path form
# //?/C:/..., and the shell globbed each one, whose `?` made it list the
# network root // (about three seconds a path on Windows, so about 80 s
# before each build started, #2596). No match was ever found, so the words
# are the same either way.
set -f

# shellcheck disable=SC2086
if ! env PATH="$TMP/cc:$PATH" "$AE" build main.ae $extras -o ./app >"$TMP/build.log" 2>&1; then
    echo "  [FAIL] win_batch_compiler_long_cmdline: build through the .cmd compiler failed"
    sed 's/^/        /' "$TMP/build.log" | head -10
    exit 1
fi
got=$(./app 2>&1 | tr -d '\r')
if [ "$got" != "tag=7 parts=25" ]; then
    echo "  [FAIL] win_batch_compiler_long_cmdline: printed '$got', expected 'tag=7 parts=25'"
    exit 1
fi

# The same build with the temp directory, where the response file goes,
# under a name with a space (a profile like C:\Users\John Doe): the `@file`
# argument must reach the compiler as one argument (#2534). A fresh cache,
# so the program is compiled again.
mkdir -p "$TMP/t m p"
rm -f ./app ./app.exe
spaced="$(cygpath -w "$TMP/t m p")"
# shellcheck disable=SC2086
if ! env PATH="$TMP/cc:$PATH" TEMP="$spaced" TMP="$spaced" AETHER_CACHE_DIR="$TMP/cache2" \
        "$AE" build main.ae $extras -o ./app >"$TMP/build2.log" 2>&1; then
    echo "  [FAIL] win_batch_compiler_long_cmdline: the build failed with a temp directory containing a space"
    sed 's/^/        /' "$TMP/build2.log" | head -10
    exit 1
fi
got=$(./app 2>&1 | tr -d '\r')
if [ "$got" != "tag=7 parts=25" ]; then
    echo "  [FAIL] win_batch_compiler_long_cmdline: with a spaced temp directory, printed '$got'"
    exit 1
fi
echo "  [PASS] win_batch_compiler_long_cmdline: a batch compiler gets a long command line through a response file"
exit 0
