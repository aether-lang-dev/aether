#!/bin/sh
# A message's strings cross to the receiving actor's thread safely (#3192,
# #3193): a field that shares a counted string with its sender is
# published at the send (its count atomic, as a module variable's is), and
# a string field is the message's own copy on both C paths, so the
# receiver's release never frees the sender's string. probe.ae reads the
# header through shim.c.
#
# The ownership grid's K6-P12D-T12 caught the first as a double free on
# Windows CI, a race that a single run on another machine may not show;
# the header shows it every time.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT"
NAME=message_field_published

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
echo "  [PASS] $NAME: shared fields published, string fields copied, on both C paths"
exit 0
