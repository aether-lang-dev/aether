#!/bin/sh
# #2688 follow-up: a fixed-size array field takes an array of its own length.
#
# Binding a whole array to an array field (`Vec { xs: ints }`, `s.names =
# arr`, `names = arr` into an actor's array state field) copies the field's
# number of elements out of the source. The typechecker let a source of
# another length through, and the copy read past the end of a shorter one:
# `Shelf { names: ["a"] }` into a `string[3]` field crashed. A source of
# another length is now a type error naming both lengths; an array literal
# may be shorter (the rest is zeroed, as for a local) but not longer; and a
# slice or a pointer, whose length is not known until the program runs, is
# refused with a hint to copy the elements one by one. A source of the
# field's own length still builds and runs.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] array_field_length: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"
fail=0

expect_error() {  # file, pattern, label
    out="$(AETHER_HOME="$ROOT" "$AETHERC" "$1" "$tmp/out.c" 2>&1)"
    if ! printf '%s' "$out" | grep -q "$2"; then
        echo "  [FAIL] array_field_length: $3"
        printf '%s\n' "$out" | grep -E "^error|-->|error:" | head -4 | sed 's/^/        /'
        fail=1
    fi
}

printf 'struct Vec { xs: int[4] }\nmain() {\n    ints = [1, 2]\n    v = Vec { xs: ints }\n    println("${v.xs[0]}")\n}\n' > "$tmp/literal_short.ae"
expect_error "$tmp/literal_short.ae" "field 'Vec.xs' is int\[4\] and cannot take int\[2\]" "a struct literal's int[4] field given an int[2] was not refused"

printf 'struct Shelf { names: string[3] }\nmain() {\n    arr = ["a", "b", "c", "d"]\n    s = Shelf { names: arr }\n    println(s.names[0])\n}\n' > "$tmp/literal_long.ae"
expect_error "$tmp/literal_long.ae" "field 'Shelf.names' is string\[3\] and cannot take string\[4\]" "a struct literal's string[3] field given a string[4] was not refused"

printf 'struct Shelf { names: string[3] }\nmain() {\n    s = Shelf { names: ["x", "y", "z"] }\n    arr = ["a"]\n    s.names = arr\n    println(s.names[0])\n}\n' > "$tmp/store_short.ae"
expect_error "$tmp/store_short.ae" "field 'Shelf.names' is string\[3\] and cannot take string\[1\]" "a store of a string[1] into a string[3] field was not refused"

printf 'struct Shelf { names: string[3] }\nmain() {\n    s = Shelf { names: ["x", "y", "z", "w"] }\n    println(s.names[0])\n}\n' > "$tmp/array_literal_long.ae"
expect_error "$tmp/array_literal_long.ae" "field 'Shelf.names' is string\[3\] and cannot take an array literal of 4 elements" "an array literal longer than the field was not refused"

printf 'struct Shelf { names: string[3] }\ntake(xs: string[]) -> int {\n    s = Shelf { names: xs }\n    return 0\n}\nmain() {\n    arr = ["a", "b", "c"]\n    println("${take(arr)}")\n}\n' > "$tmp/slice_literal.ae"
expect_error "$tmp/slice_literal.ae" "field 'Shelf.names' is string\[3\] and cannot take string\[\]: its length is not known until the program runs" "a slice given to an array field was not refused"

printf 'struct Shelf { names: string[3] }\nput(s: *Shelf, xs: string[]) {\n    s.names = xs\n}\nmain() {\n    s = heap.new(Shelf)\n    arr = ["a", "b", "c"]\n    put(s, arr)\n    heap.free(s)\n}\n' > "$tmp/slice_store.ae"
expect_error "$tmp/slice_store.ae" "field 'Shelf.names' is string\[3\] and cannot take string\[\]" "a slice stored into an array field was not refused"

cat > "$tmp/state.ae" <<'AE'
message Fill {}
actor K {
    state string[3] names
    receive {
        Fill() -> {
            names = ["p", "q", "r", "s"]
        }
    }
}
main() {
    k = spawn(K())
    k ! Fill {}
}
AE
expect_error "$tmp/state.ae" "state field 'names' is string\[3\] and cannot take an array literal of 4 elements" "an array literal longer than an array state field was not refused"

# The field's own length, and a shorter literal, build and run.
cat > "$tmp/ok.ae" <<'AE'
import std.string

struct Vec { xs: int[4] }
struct Shelf { names: string[3] }

main() {
    ints = [1, 2, 3, 4]
    v = Vec { xs: ints }
    w = Vec { xs: [5, 6] }
    arr = ["a", "b", "c"]
    s = Shelf { names: arr }
    s.names = ["x"]
    t = Shelf { names: ["p", "q", "r"] }
    t.names = arr
    println("${v.xs[3]} ${w.xs[1]} ${w.xs[3]} ${s.names[0]} ${string.length(s.names[2])} ${t.names[2]}")
}
AE
got="$("$AE" run "$tmp/ok.ae" 2>&1 | tr -d '\r' | grep -v -E "^warning|^ *-->|^ *[0-9]* [|]|^ *[|]|^Type checking|^$")"
if [ "$got" != "4 6 0 x 0 c" ]; then
    echo "  [FAIL] array_field_length: arrays of the field's length and shorter literals"
    printf 'got:\n%s\nwant:\n4 6 0 x 0 c\n' "$got" | sed 's/^/        /'
    fail=1
fi

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "  [PASS] array_field_length: an array field takes an array of its own length, a literal no longer, and no slice"
