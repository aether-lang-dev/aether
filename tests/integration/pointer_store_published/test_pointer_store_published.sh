#!/bin/sh
# A value stored through a pointer, a slice or an actor's reference is
# published (#3398, #2919), whichever part of the place it goes into: a
# struct, an array element, a whole array, an optional, nested at any depth.
# probe.ae reads the string headers through shim.c, on the default C path and
# on the one without statement expressions (AETHER_GCC_COMPAT 0).
#
# A string field reached through a pointer was published; the other parts
# took their references unpublished, so two actors that reached the same
# box counted the same strings with plain counts.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
NAME=pointer_store_published

[ -x "$AE" ] || { echo "  [SKIP] $NAME: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT
cd "$SCRIPT_DIR" || exit 1

fails=0
# run <label> <C compiler or "">: the default C path, or the one without
# statement expressions (AETHER_GCC_COMPAT 0, what MSVC compiles).
run() {
    label="$1"
    if [ -n "$2" ]; then
        AE_CC="$2" "$AE" build probe.ae -o "$TMP/probe_$label" >"$TMP/build_$label.log" 2>&1
    else
        "$AE" build probe.ae -o "$TMP/probe_$label" >"$TMP/build_$label.log" 2>&1
    fi
    if [ $? -ne 0 ]; then
        echo "  [FAIL] $NAME ($label): the probe did not build"
        sed 's/^/        /' "$TMP/build_$label.log" | head -16
        fails=$((fails + 1))
        return
    fi
    out="$("$TMP/probe_$label" 2>&1 | tr -d '\r')"
    if [ "$out" != "PASS" ]; then
        echo "  [FAIL] $NAME ($label):"
        echo "$out" | sed 's/^/        /' | head -10
        fails=$((fails + 1))
    fi
}

run default ""
run no-gcc-compat "${CC:-cc} -DAETHER_GCC_COMPAT=0"

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: every store through a pointer, a slice or a reference publishes, on both C paths"
exit 0
