#!/bin/sh
# Integration test for the v2 closure-context records in the
# `--emit=lib` metadata catalog (aether_lib_meta, schema 1.1).
# Compiles `script.ae` with `--emit=lib`, links the emitted C into a
# shared library, runs `ae lib-info`, and asserts the closure surface.
#
# Asserts:
#   1. `ae lib-info` exits 0
#   2. Schema is 1.1 (closure records present)
#   3. Closure count is 3
#   4. The `builder` record names `route` with role [builder]
#   5. The closure-typed param `action` of `each` is recorded [param]
#   6. The capturing body closure in `sum_with` records `captures
#      base: int` under role [literal]
#   7. The function table still lists the ABI exports (greet, sum_with)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "  [SKIP] lib_meta_closures: Windows DLL hosting is a follow-up"
        exit 0
        ;;
    Darwin) SO_EXT=".dylib"; SHARED_LDFLAGS="-Wl,-undefined,dynamic_lookup" ;;
    *)      SO_EXT=".so";    SHARED_LDFLAGS="" ;;
esac

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

# Compile the .ae as --emit=lib → .c (capability-free: std.string only).
if ! AETHER_HOME="$ROOT" "$AETHERC" --emit=lib \
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
        -I"$ROOT/std/math" -I"$ROOT/std/collections" \
        -I"$ROOT/std/json" \
        "$TMPDIR/script.c" $SHARED_LDFLAGS -o "$SO_PATH" \
        2>"$TMPDIR/gcc.err"; then
    echo "  [FAIL] gcc -shared:"
    head -30 "$TMPDIR/gcc.err"
    exit 1
fi

OUT="$("$AE" lib-info "$SO_PATH" 2>"$TMPDIR/lib_info.err")"
RC=$?
if [ "$RC" != "0" ]; then
    echo "  [FAIL] ae lib-info rc=$RC:"
    head -10 "$TMPDIR/lib_info.err"
    exit 1
fi

fail() { echo "  [FAIL] $1"; echo "--- output:"; echo "$OUT"; exit 1; }

echo "$OUT" | grep -q "Schema:[[:space:]]*1\\.1" || fail "schema not 1.1"
echo "$OUT" | grep -q "Closures:[[:space:]]*3" || fail "closure count not 3"

# Builder record.
echo "$OUT" | grep -qE "\\[builder\\] route" \
    || fail "missing [builder] route record"

# Closure-typed parameter record.
echo "$OUT" | grep -qE "\\[param\\] each\\.action" \
    || fail "missing [param] each.action record"

# Capturing body closure + its capture.
echo "$OUT" | grep -qE "\\[literal\\] sum_with" \
    || fail "missing [literal] sum_with record"
echo "$OUT" | grep -qE "captures base: int" \
    || fail "missing 'captures base: int' under sum_with closure"

# Function table coexists with the closure table.
echo "$OUT" | grep -qE "greet\\(string\\) -> string" \
    || fail "function table missing greet"
echo "$OUT" | grep -qE "sum_with\\(int, int\\) -> int" \
    || fail "function table missing sum_with"

echo "  [PASS] lib_meta_closures: 7/7 — schema 1.1, 3 closure records, builder/param/literal, capture, function table coexists"
