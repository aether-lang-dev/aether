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
#   4. incnext/: the same header through #include_next, whose search the
#      scan does not track: every directory, and `magic 1`.
#   5. hello with a compiler command that moves the search itself
#      (`$AE_CC` carrying -iquote, -idirafter or -include): every directory.
#   6. hello with CPATH, C_INCLUDE_PATH or OBJC_INCLUDE_PATH set: every
#      directory.
#   7. case/ (on a case-insensitive file system only): the std header
#      spelled in another case; the scan keeps its directory, the line is
#      still narrowed, and the program builds and prints `magic 1`.
#   8. hello with an @file response file in the $AE_CC command or in the
#      cflags (AETHER_CFLAGS_EXTRA), which the scan does not read: every
#      directory.
#   9. brew/ (macOS and FreeBSD only): a header found only in the Homebrew
#      (LOCALBASE) include directory ae adds includes a std header found
#      only through the list; the scan follows it there, the line is still
#      narrowed, and the program builds and prints `magic 1`.
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
unset AETHER_HOME AE_CC CPATH C_INCLUDE_PATH OBJC_INCLUDE_PATH
fails=0
fail() { echo "  [FAIL] $NAME: $1"; fails=$((fails + 1)); }

unset HOMEBREW_PREFIX LOCALBASE AETHER_CFLAGS_EXTRA
cp -r "$SCRIPT_DIR/hello.ae" "$SCRIPT_DIR/transitive" "$SCRIPT_DIR/macro" \
    "$SCRIPT_DIR/incnext" "$SCRIPT_DIR/case" "$SCRIPT_DIR/brew" "$TMP/"
cd "$TMP" || exit 1

full=$("$AE" cflags 2>/dev/null | tr ' ' '\n' | grep -c -x -- '-idirafter')
[ "$full" -gt 3 ] || { echo "  [SKIP] $NAME: ae cflags names $full -idirafter directories"; exit 0; }

# The list's directories, one a line, as `ae cflags` names them.
"$AE" cflags 2>/dev/null | tr ' ' '\n' | tr -d '"' |
    awk 'prev == "-idirafter" { print } { prev = $0 }' > "$TMP/list_dirs"

# dirs <log>: how many of the list's directories the compile line in an
# `ae build -v` log names. Only the list's: on macOS and FreeBSD the line
# also carries the Homebrew (LOCALBASE) include directory after it, which
# is not the list's and would make a line naming all of the list read as
# one more than it.
dirs() {
    grep '^\[cmd\] .* -o ' "$1" | grep -v -- ' -c ' | grep -v 'ae_avx_probe\|--version' | tail -1 |
        tr ' ' '\n' | tr -d '"' | awk 'prev == "-idirafter" { print } { prev = $0 }' |
        grep -c -x -F -f "$TMP/list_dirs"
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

for p in transitive macro incnext; do
    if ! (cd "$p" && "$AE" build -v main.ae -o main) > "$p.log" 2>&1; then
        grep -i -m3 "error" "$p.log" | cut -c1-200 | sed 's/^/        /'
        fail "$p/main.ae did not build"
        continue
    fi
    out="$("./$p/main" | tr -d '\r')"
    [ "$out" = "magic 1" ] || fail "$p/main.ae printed '$out', not 'magic 1'"
    n=$(dirs "$p.log")
    if [ "$p" = transitive ]; then
        [ "$n" -lt "$full" ] ||
            fail "transitive/: the line names every directory ($n), not the ones the scan found"
    else
        [ "$n" -eq "$full" ] ||
            fail "$p/: an include the scan cannot follow, yet the line names $n of $full directories"
    fi
done

# hello_every <tag> <what> <NAME=value>: hello, built in a cache of its own
# with NAME set to value, names every directory.
hello_every() {
    if ! env "$3" AETHER_CACHE_DIR="$TMP/cache_$1" "$AE" build -v hello.ae -o "hello_$1" > "$1.log" 2>&1; then
        grep -i -m3 "error" "$1.log" | cut -c1-200 | sed 's/^/        /'
        fail "hello did not build with $2"
        return
    fi
    n=$(dirs "$1.log")
    # -ge: an -idirafter of the command's own is counted too.
    [ "$n" -ge "$full" ] || fail "$2 moves the search, yet the line names $n of $full directories"
    [ "$(./"hello_$1" | tr -d '\r')" = "hello" ] || fail "hello printed something else with $2"
}

# The compiler command itself moving the search.
mkdir -p q
# The directory as the compiler names it (a Windows compiler takes no MSYS path).
Q="$TMP/q"
command -v cygpath > /dev/null 2>&1 && Q="$(cygpath -m "$Q")"
: > q/empty.h
CC_BASE="${CC:-gcc}"
hello_every cc_iquote "AE_CC with -iquote" "AE_CC=$CC_BASE -iquote $Q"
hello_every cc_idirafter "AE_CC with -idirafter" "AE_CC=$CC_BASE -idirafter $Q"
hello_every cc_include "AE_CC with -include" "AE_CC=$CC_BASE -include $Q/empty.h"

# The compiler's own search lists from the environment.
hello_every cpath "CPATH" "CPATH=$Q"
hello_every c_include_path "C_INCLUDE_PATH" "C_INCLUDE_PATH=$Q"
hello_every objc_include_path "OBJC_INCLUDE_PATH" "OBJC_INCLUDE_PATH=$Q"

# A response file, whose flags the scan cannot see: here it moves the search.
printf -- '-iquote %s\n' "$Q" > q/flags.rsp
hello_every cc_rsp "AE_CC with an @file" "AE_CC=$CC_BASE @$Q/flags.rsp"
hello_every cflags_rsp "an @file in the cflags" "AETHER_CFLAGS_EXTRA=@$Q/flags.rsp"

# A header only the Homebrew (macOS) or LOCALBASE (FreeBSD) include directory
# holds, which `ae` puts on the line after the list.
case "$(uname -s)" in
    Darwin) brew_var=HOMEBREW_PREFIX ;;
    FreeBSD) brew_var=LOCALBASE ;;
    *) brew_var="" ;;
esac
if [ -n "$brew_var" ]; then
    if ! (cd brew && env "$brew_var=$TMP/brew" "$AE" build -v main.ae --extra probe.c -o main) \
            > brew.log 2>&1; then
        grep -i -m3 "error" brew.log | cut -c1-200 | sed 's/^/        /'
        fail "brew/main.ae, a std header included from the $brew_var include directory, did not build"
    else
        out="$(./brew/main | tr -d '\r')"
        [ "$out" = "magic 1" ] || fail "brew/main.ae printed '$out', not 'magic 1'"
        n=$(dirs brew.log)
        [ "$n" -lt "$full" ] ||
            fail "brew/: the line names every directory ($n), not the ones the scan found"
    fi
fi

# A header spelled in another case, where the file system folds case.
: > case_probe
if [ -f CASE_PROBE ]; then
    if ! (cd case && "$AE" build -v main.ae -o main) > case.log 2>&1; then
        grep -i -m3 "error" case.log | cut -c1-200 | sed 's/^/        /'
        fail "case/main.ae, a std header spelled in another case, did not build"
    else
        out="$(./case/main | tr -d '\r')"
        [ "$out" = "magic 1" ] || fail "case/main.ae printed '$out', not 'magic 1'"
        n=$(dirs case.log)
        [ "$n" -lt "$full" ] ||
            fail "case/: the line names every directory ($n), not the ones the scan found"
    fi
fi

if [ "$fails" -ne 0 ]; then
    exit 1
fi
echo "  [PASS] $NAME: a compile names the -idirafter directories its includes reach, every one when they cannot be followed"
exit 0
