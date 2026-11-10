#!/bin/sh
# #3419: the step barrier's one word (aether_caps_slow's step bits) stays 0
# for a program that reads the environment and files but holds no restart
# claim, execution account or sandbox level, so its steps stay plain; an
# enforce block sets the sandbox bit, and a receiver stepped inline inside
# it still runs without the block's levels. cross_host.c checks the same
# across runtime copies: an actor of a library with its own runtime,
# stepped inside an enforce block of the host's copy, runs without its
# levels, whether the library had looked the levels up before or not.
#
# Fixtures, not tests: plain.ae links shim.c, and cross.ae is a library;
# the driver builds both.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/syslibs.sh"
AE="$ROOT/build/ae"
CC="${CC:-cc}"
export AETHER_HOME="$ROOT"

[ -x "$AE" ] || { echo "  [SKIP] step_word: $AE not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT
fail=0
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) ext=dll; exe=.exe; dl="" ;;
    Darwin) ext=dylib; exe=""; dl="" ;;
    Linux) ext=so; exe=""; dl="-ldl" ;;
    *) ext=so; exe=""; dl="" ;;
esac

# $1 = label, $2 = what it printed, $3 = what it should
expect() {
    if [ "$2" != "$3" ]; then
        echo "  [FAIL] step_word: $1:"
        echo "      got:"; printf '%s\n' "$2" | sed 's/^/        /'
        echo "      want:"; printf '%s\n' "$3" | sed 's/^/        /'
        fail=1
    fi
}

if ! (cd "$TMP" && "$AE" build "$SCRIPT_DIR/plain.ae" --extra "$SCRIPT_DIR/shim.c" -o "$TMP/plain" \
        > "$TMP/plain_build.log" 2>&1); then
    echo "  [FAIL] step_word: plain.ae did not build"
    tail -8 "$TMP/plain_build.log" | sed 's/^/        /'
    exit 1
fi
expect "a plain program, then an enforce block" "$(cd "$TMP" && "./plain$exe" 2>&1 | tr -d '\r')" \
"plain: getenv 1, 1000 messages, step bits 0
enforce: the receiver read PATH 1, the block itself 0, step bits now 8"

if ! (cd "$TMP" && "$AE" build --emit=lib --with=extern,os "$SCRIPT_DIR/cross.ae" \
        --extra "$SCRIPT_DIR/cross_hooks.c" -o "$TMP/libcross.$ext" > "$TMP/cross_build.log" 2>&1); then
    echo "  [FAIL] step_word: cross.ae did not build"
    tail -8 "$TMP/cross_build.log" | sed 's/^/        /'
    exit 1
fi
# shellcheck disable=SC2046
if ! "$CC" -O1 -DAETHER_HAS_SANDBOX -I"$ROOT/include" -I"$ROOT/runtime" $("$AE" cflags --cflags) \
        "$SCRIPT_DIR/cross_host.c" -o "$TMP/cross_host" $("$AE" cflags --libs) $dl > "$TMP/cc.log" 2>&1; then
    echo "  [FAIL] step_word: cross_host.c did not build"
    head -8 "$TMP/cc.log" | sed 's/^/        /'
    exit 1
fi
for first in 1 0; do
    expect "across runtime copies, the library looked up first: $first" \
        "$("$TMP/cross_host" "$TMP/libcross.$ext" $first 2>"$TMP/cross$first.err" | tr -d '\r')" \
        "cross: the host's own check inside its block 0; the actor was stepped inside it, and read PATH 1"
done
# A third copy loaded first owns the count, on every platform (#3438): the
# library, which looked nothing up, joins it before its first step. (On
# macOS the executable's own copy is found first, so the cases above are
# this one there.)
cp "$TMP/libcross.$ext" "$TMP/libfirst.$ext"
for first in 1 0; do
    expect "across three runtime copies, the library looked up first: $first" \
        "$("$TMP/cross_host" "$TMP/libcross.$ext" $first "$TMP/libfirst.$ext" 2>"$TMP/third$first.err" | tr -d '\r')" \
        "cross: the host's own check inside its block 0; the actor was stepped inside it, and read PATH 1"
done

if [ "$fail" = 0 ]; then
    echo "  [PASS] step_word: a program that reads the environment and files keeps its steps plain (step bits 0); an enforce block sets the sandbox bit and a receiver stepped inside it, in this copy or another, runs without its levels"
fi
exit $fail
