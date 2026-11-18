#!/bin/sh
# A file that ends inside a block is a syntax error, said at the `{` that
# opened the block. aetherc took the end of the file for the missing `}` and
# compiled a function body, an `if` body or a `main` with its last brace
# gone (nib-code's .tests.ae lost one and aeb then ran 72 of its 82 tests,
# quietly). A block that does close still compiles.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
NAME=parser_unclosed_block_at_eof

if [ ! -x "$AETHERC" ] && [ ! -x "$AETHERC.exe" ]; then
    echo "  [SKIP] $NAME: $AETHERC not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

# refused <case> <line:col of the opening brace>: the file in $TMP/<case>.ae
# fails to compile with the unclosed-block error at that position.
refused() {
    if "$AETHERC" "$TMP/$1.ae" "$TMP/$1.c" >"$TMP/$1.log" 2>&1; then
        echo "  [FAIL] $NAME/$1: compiled; the file ends inside a block"
        fails=$((fails + 1))
        return
    fi
    if ! grep -q "unclosed '{'" "$TMP/$1.log" || ! grep -q "$1.ae:$2" "$TMP/$1.log"; then
        echo "  [FAIL] $NAME/$1: expected the unclosed-block error at $2, got:"
        sed 's/^/        /' "$TMP/$1.log" | head -8
        fails=$((fails + 1))
    fi
}

# The repro from the aeb 72/82 hunt: a function's body loses its `}`.
printf 'f(x: int) -> int {\n    if x > 0 {\n        println("a")\n    }\n    return 0\n' > "$TMP/fn_body.ae"
refused fn_body 1:18

# main loses its `}` after a closed `if`: the outer brace is the open one.
printf 'main() {\n    if true {\n        println("hi")\n    }\n' > "$TMP/main_after_if.ae"
refused main_after_if 1:8

# The inner block is the one left open.
printf 'main() {\n    if true {\n        println("hi")\n' > "$TMP/inner_if.ae"
refused inner_if 2:13

# Closed, it compiles.
printf 'main() {\n    if true {\n        println("hi")\n    }\n}\n' > "$TMP/closed.ae"
if ! "$AETHERC" "$TMP/closed.ae" "$TMP/closed.c" >"$TMP/closed.log" 2>&1; then
    echo "  [FAIL] $NAME/closed: a closed block failed to compile:"
    sed 's/^/        /' "$TMP/closed.log" | head -8
    fails=$((fails + 1))
fi

if [ "$fails" -ne 0 ]; then
    exit 1
fi
echo "  [PASS] $NAME: a file ending inside a block is refused at its opening brace"
exit 0
