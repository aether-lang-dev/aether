#!/bin/sh
# A type written for a local bound in several branches is its type (#3070).
#
# A name first bound inside a branch or loop body is one variable for the
# whole function, with the join of its bindings. A binding that wrote its
# type was joined like the others, so the written type was dropped or
# refused by the order the two were written in: `uint64 x = 7` beside
# `x = n` (an int) was refused, the other order compiled as an int, and
# `x = -1` beside it printed -1; `int x = 0` beside `x = 1.5` made a float,
# `uint8 x = 7` beside `x = 300` an int. The written type is the local's
# type now, in either order, and a binding of a value it does not hold, or
# one that writes another type, is refused at that binding. A value it
# holds still binds, and the local is the written type.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] written_type_sibling_branch: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"
cd "$tmp" || exit 1

fail() {
    echo "  [FAIL] written_type_sibling_branch: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2" | head -20
    exit 1
}

# program <statement>: main.ae binds `x` by <statement> in f, then reads it.
program() {
    cat > main.ae <<AE
f(k: int, n: int) {
    $1
    println("\${x}")
}
main() {
    f(1, -1)
}
AE
}

# expect_reject <name> <want-message> <want-position>: main.ae must fail
# type checking once, with that message at that position.
expect_reject() {
    if "$AE" check main.ae > "$1.log" 2>&1; then
        fail "$1: accepted" "$1.log"
    fi
    tr -d '\r' < "$1.log" > "$1.txt"
    grep -qF "$2" "$1.txt" || fail "$1: missing '$2'" "$1.txt"
    grep -qF -- "--> main.ae:$3" "$1.txt" || fail "$1: not reported at $3" "$1.txt"
    [ "$(grep -c '^error' "$1.txt")" = 1 ] || fail "$1: not reported once" "$1.txt"
}

HELD="it is written as uint64 in another branch or loop body of this function, which does not hold the value"

program 'if k > 0 { uint64 x = 7 } else { x = n }'
expect_reject written_first "cannot bind 'x' to this int value: $HELD" "2:38"

program 'if k > 0 { x = n } else { uint64 x = 7 }'
expect_reject written_second "cannot bind 'x' to this int value: $HELD" "2:16"

program 'if k > 0 { x = -1 } else { uint64 x = 7 }'
expect_reject negative_literal "cannot bind 'x' to this int value: $HELD" "2:16"

program 'if k > 0 { x = 1.5 } else { int x = 0 }'
expect_reject float_in_int "cannot bind 'x' to this float value: it is written as int" "2:16"

program 'if k > 0 { uint8 x = 7 } else { x = 300 }'
expect_reject literal_past_uint8 "cannot bind 'x' to this int value: it is written as uint8" "2:37"

program 'if k > 0 { int x = 0 } else { long x = 1 }'
expect_reject two_written "cannot write 'x' as long: it is written as int in another branch" "2:40"

# A written type of another kind than the other binding's is the local's
# type too: `x = "s"` beside `int x = 1` made `x` a `const char*`, and the
# int branch stored 1 into it, a crash when it ran. Refused at the string,
# in either order.
program 'if k > 0 { x = "s" } else { int x = 1 }'
expect_reject string_beside_int "cannot bind 'x' to this string value: it is written as int" "2:16"

program 'if k > 0 { int x = 1 } else { x = "s" }'
expect_reject int_beside_string "cannot bind 'x' to this string value: it is written as int" "2:35"

program 'match k { 0 -> { x = -1 } _ -> { uint64 x = 7 } }'
expect_reject match_arms "cannot bind 'x' to this int value: $HELD" "2:22"

cat > main.ae <<'AE'
f(n: int) {
    i = 0
    while i < 2 {
        if i == 0 { y = n } else { uint64 y = 7 }
        i = i + 1
    }
    println("${y}")
}
main() {
    f(-1)
}
AE
expect_reject loop_body "cannot bind 'y' to this int value: it is written as uint64" "4:21"

# A value the written type holds binds, in either order, and the local is
# the written type: a uint64 past a long's range and a long past an int's
# come back whole.
cat > main.ae <<'AE'
big(k: int) {
    if k > 0 { x = 5 } else { uint64 x = 18446744073709551615 }
    return x
}
wide(k: int, n: int) {
    if k > 0 { long x = 5000000000 } else { x = n }
    return x
}
small(k: int) {
    if k > 0 { x = 5 } else { uint8 x = 250 }
    return x
}
main() {
    println("${big(1)} ${big(0)} ${wide(1, 3)} ${wide(0, -3)} ${small(1)} ${small(0)}")
}
AE
want="5 18446744073709551615 5000000000 -3 5 250"
got="$("$AE" run main.ae 2>&1 | tr -d '\r' | grep -v -E "^warning|^hint|^ *-->|^ *[0-9]* [|]|^ *[|]|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] written_type_sibling_branch: a value the written type holds"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi

echo "  [PASS] written_type_sibling_branch: a written type is the local's type in either order; a value it does not hold is refused"
