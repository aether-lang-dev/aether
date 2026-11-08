#!/bin/sh
# Regression: has_return_value must stop at AST_CLOSURE boundaries so a
# nested lambda's `return` doesn't bubble up and mis-type the enclosing
# closure. Without the fix, the outer closure is declared `static int`
# but has no return statement; gcc flags this with -Wreturn-type.
#
# Compile the generated C with -Wreturn-type -Werror and require a
# clean build. If the bug regresses, this test goes red.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

SRC="$SCRIPT_DIR/nested_return.ae"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

C_FILE="$TMPDIR/out.c"
if ! "$AETHERC" "$SRC" "$C_FILE" >"$TMPDIR/cc.log" 2>&1; then
    echo "  [FAIL] aetherc codegen"
    cat "$TMPDIR/cc.log"
    exit 1
fi

# Compile the generated C with strict return-type checking. The fix
# being regressed turns a nested lambda's return into the enclosing
# closure's return type, causing a static int with no return path.
# `-Werror=return-type` alone (not a blanket -Wall -Wextra -Werror)
# avoids blessing the compiler's other warning decisions.
if ! ${CC:-cc} -Werror=return-type \
         -I"$ROOT/runtime" -I"$ROOT/runtime/actors" \
         -I"$ROOT/std" -I"$ROOT/std/io" -I"$ROOT/std/collections" \
         -c "$C_FILE" -o "$TMPDIR/out.o" 2>"$TMPDIR/gcc.log"; then
    echo "  [FAIL] gcc -Werror=return-type on generated C"
    cat "$TMPDIR/gcc.log" | head -20
    exit 1
fi

echo "  [PASS] closure_nested_return: outer closure is correctly void"
