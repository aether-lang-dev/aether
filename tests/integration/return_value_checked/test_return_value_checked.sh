#!/bin/sh
# #2337: a returned value is checked by the expression rules.
#
# `return` walked its value through the statement path, which only visited
# children, so no expression rule ran on anything returned: `return (s as
# int) & 255` with a string `s` compiled (the C compiler then warned about a
# pointer-to-int cast and the program printed an address), while the same
# cast assigned to a local was refused with E0200.
#
# Asserts the invalid cast is refused in a return, alone and nested, with the
# same message an assignment gets.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || { echo "  [SKIP] return_value_checked: ae not built"; exit 0; }

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

expect_refused() {
    # $1: label, $2: function body line
    printf 'abs_of(x: int) -> int {\n    return x\n}\n\nbad(s: string) -> int {\n    %s\n}\n\nmain() {\n    println("${bad("x")}")\n}\n' "$2" > "$T/bad.ae"
    OUT=$("$AE" check "$T/bad.ae" 2>&1)
    RC=$?
    if [ $RC -eq 0 ]; then
        echo "  [FAIL] return_value_checked: $1 was accepted"
        printf '%s\n' "$OUT" | sed 's/^/    /' | head -10
        exit 1
    fi
    case "$OUT" in
        *"cannot cast string to int with \`as\`"*) ;;
        *) echo "  [FAIL] return_value_checked: $1 failed for another reason"
           printf '%s\n' "$OUT" | sed 's/^/    /' | head -10
           exit 1 ;;
    esac
}

expect_refused "a cast returned alone" 'return s as int'
expect_refused "a cast inside a returned expression" 'return (s as int) & 255'
expect_refused "a cast inside a returned call argument" 'return abs_of(s as int)'

echo "  [PASS] return_value_checked: an invalid cast in a return is refused, as when assigned"
