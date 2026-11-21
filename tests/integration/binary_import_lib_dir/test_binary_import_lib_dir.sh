#!/bin/sh
# AETHER_LIB_DIR finds a binary library by the same rule --lib does: by its
# file name, lib<name>.so (.dylib, .dll), whatever source it was built from.
#
# The binary-import prepass probes tc.lib_dirs for lib<name>.*, and
# AETHER_LIB_DIR joined tc.lib_dirs only when the cache key was computed,
# after that prepass: the variable was never searched for a binary library
# (a library named after its source seemed to work, through the source file
# beside it). And a project with [dependencies] dropped the variable
# altogether, since any entry already in the chain counted as a typed --lib.
# The variable is now read once per command, before anything joins the
# chain (tc_lib_dirs_seed_from_env), so every probe sees it.
#
# Found with RexxAE, whose librexxae is built from src/engine.ae
# (aether-guest-languages asks/ae-lib-dir-binary-library-by-file-name.md).
#
# Asserts, with libother built from tiny.ae and no source of either beside
# the program: `--lib out`, `AETHER_LIB_DIR=out` (relative and absolute)
# for `ae build`, `ae run` and `ae check`; the same from a project with a
# [dependencies] entry; and `ae lib-path` naming the directory.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] binary_import_lib_dir: ae not built"; exit 0; }

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) SO_EXT=".dll"; EXE=".exe" ;;
    Darwin) SO_EXT=".dylib"; EXE="" ;;
    *)      SO_EXT=".so"; EXE="" ;;
esac

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK" || true' EXIT
fails=0
bad() { echo "  [FAIL] binary_import_lib_dir: $1"; fails=$((fails + 1)); }

mkdir -p "$WORK/build" "$WORK/app/out"
cp "$SCRIPT_DIR/tiny.ae" "$WORK/build/"
cp "$SCRIPT_DIR/use.ae" "$WORK/app/"
export AETHER_HOME="$ROOT"
unset AETHER_LIB_DIR
if ! (cd "$WORK/build" && "$AE" build --emit=lib tiny.ae -o "$WORK/app/out/libother$SO_EXT") \
        >"$WORK/lib.log" 2>&1; then
    cat "$WORK/lib.log"
    echo "  [FAIL] binary_import_lib_dir: ae build --emit=lib tiny.ae"
    exit 1
fi
cd "$WORK/app"

# built <label> <exe>: the build said nothing went wrong and the program prints 42.
built() {
    if [ "$(./"$2$EXE" 2>&1)" != 42 ]; then
        bad "$1: the program does not print 42"
    fi
}

"$AE" build use.ae --lib out -o a >a.log 2>&1 || { cat a.log; bad "--lib out: ae build failed"; }
built "--lib out" a

AETHER_LIB_DIR=out "$AE" build use.ae -o b >b.log 2>&1 || { cat b.log; bad "AETHER_LIB_DIR=out: ae build failed"; }
built "AETHER_LIB_DIR=out" b

AETHER_LIB_DIR="$WORK/app/out" "$AE" build use.ae -o c >c.log 2>&1 \
    || { cat c.log; bad "AETHER_LIB_DIR=<absolute>: ae build failed"; }
built "AETHER_LIB_DIR=<absolute>" c

out=$(AETHER_LIB_DIR=out "$AE" run use.ae 2>&1)
[ "$out" = 42 ] || bad "AETHER_LIB_DIR=out: ae run printed '$out'"

AETHER_LIB_DIR=out "$AE" check use.ae >check.log 2>&1 \
    || { cat check.log; bad "AETHER_LIB_DIR=out: ae check failed"; }

paths=$(AETHER_LIB_DIR=out "$AE" lib-path 2>&1)
case "$paths" in
    *out*) ;;
    *) bad "AETHER_LIB_DIR=out: ae lib-path does not name out: $paths" ;;
esac

# A project with a dependency: the dependency's modules join the chain, and
# AETHER_LIB_DIR still does, ahead of them, as `ae lib-path` documents.
PKGS="$WORK/home/.aether/packages/example.com/acme/widgets"
mkdir -p "$PKGS/frontend" "$WORK/proj"
printf '[package]\nname = "widgets"\nmodules = "frontend"\n' > "$PKGS/aether.toml"
printf 'exports(paint)\n\npaint() -> string {\n    return "painted"\n}\n' > "$PKGS/frontend/module.ae"
printf '[package]\nname = "consumer"\n\n[dependencies]\n"example.com/acme/widgets" = "1.0.0"\n' \
    > "$WORK/proj/aether.toml"
printf 'import frontend\nimport other\n\nmain() {\n    println(frontend.paint())\n    println("${other.twice(21)}")\n}\n' \
    > "$WORK/proj/use.ae"
HOME_DIR="$WORK/home"
LIBDIR="$WORK/app/out"
if command -v cygpath >/dev/null 2>&1; then
    HOME_DIR="$(cygpath -m "$HOME_DIR")"
    LIBDIR="$(cygpath -m "$LIBDIR")"
fi
out=$(cd "$WORK/proj" && HOME="$WORK/home" USERPROFILE="$HOME_DIR" AETHER_LIB_DIR="$LIBDIR" \
      "$AE" run use.ae 2>&1 | tr '\n' ' ' | sed 's/ *$//')
[ "$out" = "painted 42" ] || bad "a project with [dependencies], AETHER_LIB_DIR=<absolute>: ae run printed '$out'"

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] binary_import_lib_dir: AETHER_LIB_DIR and --lib find a binary library by its file name"
exit 0
