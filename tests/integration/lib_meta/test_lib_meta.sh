#!/bin/sh
# Integration test for the `--emit=lib` symbol-catalog metadata
# (issue #403). Compiles `script.ae` with `--emit=lib`, links the
# emitted C into a shared library, then runs `ae lib-info` against
# the produced artifact and asserts every field of the printed
# metadata.
#
# Asserts:
#   1. `ae lib-info` exits 0
#   2. Header reports schema 1.0
#   3. Header names the source file
#   4. Function count is 3
#   5. Each of the three exports is listed with its expected name
#      and signature
#   6. The @c_callback export has c_symbol == aether_name (no
#      double-prefix)
#   7. The plain exports show `c_symbol: aether_<name>` (since
#      different from aether_name)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "  [SKIP] lib_meta: Windows DLL hosting is a follow-up"
        exit 0
        ;;
    Darwin) SO_EXT=".dylib"; SHARED_LDFLAGS="-Wl,-undefined,dynamic_lookup" ;;
    *)      SO_EXT=".so";    SHARED_LDFLAGS="" ;;
esac

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

# Compile the .ae as --emit=lib → .c
if ! AETHER_HOME="$ROOT" "$AETHERC" --emit=lib --with=net \
        "$SCRIPT_DIR/script.ae" "$TMPDIR/script.c" \
        2>"$TMPDIR/aetherc.err"; then
    echo "  [FAIL] aetherc --emit=lib:"
    head -30 "$TMPDIR/aetherc.err"
    exit 1
fi

# Compile the emitted C into a shared library.
SO_PATH="$TMPDIR/script$SO_EXT"
if ! gcc -fPIC -shared -O2 \
        -I"$ROOT/runtime" -I"$ROOT/runtime/actors" \
        -I"$ROOT/runtime/scheduler" -I"$ROOT/runtime/utils" \
        -I"$ROOT/runtime/memory" -I"$ROOT/runtime/config" \
        -I"$ROOT/std" -I"$ROOT/std/string" -I"$ROOT/std/io" \
        -I"$ROOT/std/math" -I"$ROOT/std/net" -I"$ROOT/std/collections" \
        -I"$ROOT/std/json" \
        "$TMPDIR/script.c" $SHARED_LDFLAGS -o "$SO_PATH" \
        2>"$TMPDIR/gcc.err"; then
    echo "  [FAIL] gcc -shared:"
    head -30 "$TMPDIR/gcc.err"
    exit 1
fi

# Run `ae lib-info` against the artifact and capture its dump.
OUT="$("$AE" lib-info "$SO_PATH" 2>"$TMPDIR/lib_info.err")"
RC=$?
if [ "$RC" != "0" ]; then
    echo "  [FAIL] ae lib-info rc=$RC:"
    head -10 "$TMPDIR/lib_info.err"
    exit 1
fi

# Assert each piece of the dump.
fail() { echo "  [FAIL] $1"; echo "--- output:"; echo "$OUT"; exit 1; }

echo "$OUT" | grep -q "Aether Library:" || fail "missing 'Aether Library:' header"
echo "$OUT" | grep -q "Schema:[[:space:]]*1\\.0" || fail "schema not 1.0"
echo "$OUT" | grep -q "Source:" || fail "missing 'Source:' line"
echo "$OUT" | grep -q "Functions:[[:space:]]*3" || fail "function count not 3"

# The three exports.
echo "$OUT" | grep -qE "aether_script_handle\\(ptr, ptr, ptr\\) -> void" \
    || fail "aether_script_handle entry missing or wrong signature"
echo "$OUT" | grep -qE "double_int\\(int\\) -> int" \
    || fail "double_int entry missing or wrong signature"
echo "$OUT" | grep -qE "greet\\(string\\) -> string" \
    || fail "greet entry missing or wrong signature"

# @c_callback export should NOT show a `c_symbol:` redundancy line
# (its aether_name == c_symbol). Plain exports should show one.
script_block=$(echo "$OUT" | sed -n '/aether_script_handle/,/@/p')
echo "$script_block" | grep -q "c_symbol:" \
    && fail "@c_callback export should NOT show c_symbol:"

double_block=$(echo "$OUT" | sed -n '/double_int/,/@/p')
echo "$double_block" | grep -q "c_symbol:[[:space:]]*aether_double_int" \
    || fail "double_int should show c_symbol: aether_double_int"

# Source-file references show up.
echo "$OUT" | grep -qE "@.*script\\.ae:[0-9]+" \
    || fail "no @ source-file:line references"

echo "  [PASS] lib_meta: 7/7 — schema, source, function count, three exports, c_symbol gating, source refs"
