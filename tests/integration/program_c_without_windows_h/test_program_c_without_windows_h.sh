#!/bin/sh
# #2673: a program's generated C, preprocessed for Windows, has no windows.h
# in it. mingw-w64's winnt.h includes <x86intrin.h> and with it every AVX-512
# and AVX10 header GCC ships, some 80,000 lines that were most of each test
# build's compile time on Windows. Checked for a program without actors (the
# prelude included windows.h itself) and one with them (the thread layer,
# which a program now gets in its types-only form), and both are built and
# run where the host can.
#
# Preprocessed with the host's gcc on a Windows host, else with
# `zig cc -target x86_64-windows-gnu` when zig is on PATH, else skipped.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

fail() { echo "  [FAIL] program_c_without_windows_h: $1"; exit 1; }

on_windows=0
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) on_windows=1; PP="gcc" ;;
    *)
        if command -v zig >/dev/null 2>&1; then
            PP="zig cc -target x86_64-windows-gnu"
        else
            echo "  [SKIP] program_c_without_windows_h: no Windows preprocessor (not a Windows host, no zig)"
            exit 0
        fi ;;
esac

# The include and define flags only: -E needs no libraries, and zig resolves
# -l flags even then.
flags=""
set -- $("$AE" cflags)
while [ $# -gt 0 ]; do
    case "$1" in
        -idirafter|-isystem|-I|-D) flags="$flags $1 $2"; shift 2 ;;
        -I*|-D*) flags="$flags $1"; shift ;;
        *) shift ;;
    esac
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

for prog in plain actors; do
    "$AETHERC" "$SCRIPT_DIR/$prog.ae" "$TMP/$prog.c" > "$TMP/$prog.aetherc" 2>&1 \
        || { cat "$TMP/$prog.aetherc"; fail "aetherc could not compile $prog.ae"; }
    $PP $flags -E "$TMP/$prog.c" -o "$TMP/$prog.i" 2> "$TMP/$prog.pp" \
        || { head -20 "$TMP/$prog.pp"; fail "the $prog program's C does not preprocess for Windows"; }
    if grep -q 'windows\.h"' "$TMP/$prog.i"; then
        fail "windows.h is in the $prog program's C"
    fi
    if grep -q 'x86intrin\.h"' "$TMP/$prog.i"; then
        fail "x86intrin.h is in the $prog program's C"
    fi
    if [ "$on_windows" = 1 ]; then
        out="$("$AE" run "$SCRIPT_DIR/$prog.ae" 2>&1)" || { echo "$out"; fail "$prog.ae did not run"; }
        echo "$out" | grep -q "^$prog " || { echo "$out"; fail "$prog.ae printed the wrong thing"; }
    fi
done

echo "  [PASS] program_c_without_windows_h: no windows.h in a program's C, with or without actors"
