#!/bin/sh
# Issue #2218: a local variable spelt like another module's exported
# function pulled that function into the build. The dead-code prune seeded
# every bare identifier as a possible function reference, and the suffix
# match that serves glob imports turned glyphs' local `record` into a use of
# ui's `ui_record`. That builder returns its `void*` `_builder` from an
# `-> int` function, which gcc 14 rejects under -Werror=int-conversion, so
# a program that never called ui.record stopped compiling.
#
# Acceptance:
#   1. main.ae (never calls ui.record) emits no ui_record at all, and runs.
#   2. calls_record.ae (does call it) emits C that compiles clean under
#      -Werror=int-conversion, because `return _builder` is cast, and runs.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
AE="$ROOT/build/ae"
NAME=local_named_like_imported_fn

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
cd "$SCRIPT_DIR" || exit 1

inc="-I$ROOT/runtime -I$ROOT/runtime/actors -I$ROOT/std -I$ROOT/std/collections"

emit_and_compile() {
    src="$1"
    if ! "$AETHERC" "$src" "$tmpdir/$src.c" >"$tmpdir/emit.log" 2>&1; then
        echo "  [FAIL] $NAME: aetherc failed on $src"
        sed 's/^/    /' "$tmpdir/emit.log" | head -20
        exit 1
    fi
    if ! gcc -c -Werror=int-conversion $inc "$tmpdir/$src.c" -o "$tmpdir/$src.o" >"$tmpdir/gcc.log" 2>&1; then
        echo "  [FAIL] $NAME: gcc -Werror=int-conversion rejected the C for $src"
        sed 's/^/    /' "$tmpdir/gcc.log" | head -20
        exit 1
    fi
}

expect_output() {
    src="$1"; want="$2"
    out=$(AETHER_HOME="" "$AE" run "$src" 2>&1)
    got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
    if [ "$got" != "$want" ]; then
        echo "  [FAIL] $NAME: $src expected '$want', got '$got'"
        printf '%s\n' "$out" | head -20 | sed 's/^/    /'
        exit 1
    fi
}

emit_and_compile main.ae
if grep -q "ui_record" "$tmpdir/main.ae.c"; then
    echo "  [FAIL] $NAME: glyphs' local 'record' kept ui_record in the build"
    grep -n "ui_record" "$tmpdir/main.ae.c" | head -5 | sed 's/^/    /'
    exit 1
fi
expect_output main.ae "1 45"

emit_and_compile calls_record.ae
expect_output calls_record.ae "7"

echo "  [PASS] $NAME: a local spelt like an exported fn no longer drags it in; int builder returning _builder casts"
exit 0
