#!/bin/sh
# A returned value fits the result type (#2782).
#
# The checker checked a returned expression on its own and never asked
# whether it fits the result type, as it asks of a typed binding's value, so
# every mismatch reached the C compiler: `word(n: int) -> string { return n }`
# was "returning 'int' from a function with return type 'const char *'". A
# value of another kind is now the checker's error at the `return`, naming
# both types, for a written result type and an inferred one, in a function,
# a closure and a call's trailing block, value by value in a multi-value
# return and arm by arm in a branching value. A later `return` that an
# inferred int result would truncate is the narrowing error an inferred int
# local gives. What C cannot do with a value is refused although a typed
# binding lets it through. The conversions a return keeps are
# tests/regression/test_return_conversions.ae.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] return_type_check: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

# refused <name> <line:col> <text>: `ae check` refuses <name>.ae with E0200
# at that position, its message containing <text>.
refused() {
    name="$1"; at="$2"; text="$3"
    out=$(cd "$TMP" && "$AE" check "$name.ae" 2>&1)
    if [ $? -eq 0 ]; then
        echo "  [FAIL] $name: accepted"
        echo "$out" | sed 's/^/        /'
        fails=$((fails + 1)); return
    fi
    case "$out" in
        *"error[E0200]"*"$text"*"$name.ae:$at"*) ;;
        *) echo "  [FAIL] $name: not E0200 \"$text\" at $at:"
           echo "$out" | sed 's/^/        /'
           fails=$((fails + 1)) ;;
    esac
}

cat > "$TMP/int_as_string.ae" <<'AE'
word(n: int) -> string {
    return n
}

main() {
    println(word(1))
}
AE
refused int_as_string 2:12 "'word' returns string, but this value is int"

cat > "$TMP/string_as_int.ae" <<'AE'
count(s: string) -> int {
    return s
}

main() {
    println("${count("a")}")
}
AE
refused string_as_int 2:12 "'count' returns int, but this value is string"

cat > "$TMP/other_struct.ae" <<'AE'
struct Apple {
    w: int
}

struct Pear {
    w: int
}

pick() -> Apple {
    return Pear { w: 1 }
}

main() {
    println("${pick().w}")
}
AE
refused other_struct 10:12 "'pick' returns Apple, but this value is Pear"

cat > "$TMP/enum_as_string.ae" <<'AE'
enum Hue {
    Red,
    Green
}

label() -> string {
    return Hue.Green
}

main() {
    println(label())
}
AE
refused enum_as_string 7:15 "'label' returns string, but this value is Hue"

cat > "$TMP/tuple_position.ae" <<'AE'
both() -> (int, string) {
    return 1, 2
}

main() {
    a, b = both()
    println("${a} ${b}")
}
AE
refused tuple_position 2:15 "'both' returns (int, string), and value 2 here is int, not string"

cat > "$TMP/if_arm.ae" <<'AE'
name(c: bool) -> string {
    return if c { "yes" } else { 0 }
}

main() {
    println(name(true))
}
AE
refused if_arm 2:34 "'name' returns string, but this value is int"

cat > "$TMP/match_arm.ae" <<'AE'
code(n: int) -> int {
    return match n {
        1 -> 10
        _ -> "many"
    }
}

main() {
    println("${code(2)}")
}
AE
refused match_arm 4:14 "'code' returns int, but this value is string"

cat > "$TMP/trailing_block.ae" <<'AE'
builder run(label: string) {
}

title(n: int) -> string {
    run("x") {
        if n > 0 {
            return n
        }
    }
    return "none"
}

main() {
    println(title(1))
}
AE
refused trailing_block 7:20 "'title' returns string, but this value is int"

cat > "$TMP/inferred_kind.ae" <<'AE'
pick(n: int) {
    if n > 0 {
        return 1
    }
    return "s"
}

main() {
    println("${pick(1)}")
}
AE
refused inferred_kind 5:12 "'pick' returns int (inferred from its first \`return\`), but this value is string"

cat > "$TMP/inferred_narrowing.ae" <<'AE'
half(n: int) {
    if n > 0 {
        return 1
    }
    return 2.5
}

main() {
    println("${half(0)}")
}
AE
refused inferred_narrowing 5:12 "narrowing return from 'half': its result type was inferred as int from its first \`return\`, but this \`return\` gives a float"

cat > "$TMP/inferred_long.ae" <<'AE'
big(n: int) {
    let v: long = 5000000000
    if n > 0 {
        return 1
    }
    return v
}

main() {
    println("${big(0)}")
}
AE
refused inferred_long 6:12 "gives a 64-bit value, which would truncate"

cat > "$TMP/closure_kind.ae" <<'AE'
main() {
    c = |n: int| {
        if n > 0 {
            return 1
        }
        return "s"
    }
    println("${c(1)}")
}
AE
refused closure_kind 4:20 "this closure returns string"

cat > "$TMP/closure_narrowing.ae" <<'AE'
main() {
    c = |n: int| {
        if n > 0 {
            return 1
        }
        return 2.5
    }
    println("${c(0)}")
}
AE
refused closure_narrowing 6:16 "narrowing return from a closure"

cat > "$TMP/closure_as_ptr.ae" <<'AE'
keep() -> ptr {
    return |k: int| {
        return k
    }
}

main() {
    p = keep()
    println("${p == null}")
}
AE
refused closure_as_ptr 2:12 "'keep' returns ptr, but this value is closure"

cat > "$TMP/null_as_value_optional.ae" <<'AE'
find(n: int) -> int? {
    if n > 0 {
        return n
    }
    return null
}

main() {
    println("${find(0) ?? 7}")
}
AE
refused null_as_value_optional 5:12 "'find' returns int?, but this value is ptr"

cat > "$TMP/empty_as_int.ae" <<'AE'
size() -> int {
    return []
}

main() {
    println("${size()}")
}
AE
refused empty_as_int 2:12 "'size' returns int, but this value is the empty slice"

# #3044: one value from a `T!` function is its success value, checked
# against T; a `string` returned from an `int!` function reached C. An arm
# of a branching value is checked the same way.
cat > "$TMP/string_as_int_result.ae" <<'AE'
f(s: string) -> int! {
    if s == "" {
        return s
    }
    return 1
}

main() {
    v = f("") or -1
    println("${v}")
}
AE
refused string_as_int_result 3:16 "'f' returns int!, and this value is string, not int"

cat > "$TMP/result_arm.ae" <<'AE'
f(c: bool) -> int! {
    return if c { 1 } else { "two" }
}

main() {
    v = f(true) or -1
    println("${v}")
}
AE
refused result_arm 2:30 "'f' returns int!, and this value is string, not int"

# The shape that once returned a stale value (worked around in OpenDisk-ae's
# native_capacity): an early `return 0, 0, 0` ahead of the real multi-value
# return of 64-bit values. The result was typed from the first return, and
# the 64-bit values were cut to it with no diagnostic. It is the narrowing
# error now; the written result type that keeps them is
# tests/regression/test_tuple_early_return_written_result.ae.
cat > "$TMP/tuple_early_return.ae" <<'AE'
total() -> long { return 245000000000 }

capacity(ok: int) -> {
    if ok != 1 { return 0, 0, 0 }
    return total(), total(), 1
}

main() {
    t, a, k = capacity(1)
    println("${t} ${a} ${k}")
}
AE
refused tuple_early_return 5:12 "narrowing return from 'capacity': its result type was inferred as int from its first \`return\`, but this \`return\` gives a 64-bit value"

# A multi-value `return` with another count than the result: one value
# first and then two, or two and then three, or three and then two. They
# were let through and lowered to the first `return`'s shape: a tuple into
# an int result reached the C compiler, the extra value of a longer tuple
# was dropped (a C warning only) and a shorter one zero-filled the rest.
cat > "$TMP/one_then_two.ae" <<'AE'
pick(ok: int) -> {
    if ok == 0 { return 0 }
    return 41, 1
}

main() {
    x = pick(1)
    println("${x}")
}
AE
refused one_then_two 3:5 "'pick' returns int (inferred from its first \`return\`), but this \`return\` gives 2 values"

cat > "$TMP/two_then_three.ae" <<'AE'
pick(ok: int) -> {
    if ok == 0 { return 0, "" }
    return 41, "", 3
}

main() {
    x, e = pick(1)
    println("${x} ${e}")
}
AE
refused two_then_three 3:5 "'pick' returns (int, string) (inferred from its first \`return\`), but this \`return\` gives 3 values"

cat > "$TMP/three_then_two.ae" <<'AE'
pick(ok: int) -> {
    if ok == 0 { return 0, 0, 0 }
    return 41, 1
}

main() {
    a, b, c = pick(1)
    println("${a} ${b} ${c}")
}
AE
refused three_then_two 3:5 "'pick' returns (int, int, int) (inferred from its first \`return\`), but this \`return\` gives 2 values"

cat > "$TMP/written_count.ae" <<'AE'
pick() -> (int, string) {
    return 41, "", 3
}

main() {
    x, e = pick()
    println("${x} ${e}")
}
AE
refused written_count 2:5 "'pick' returns (int, string), but this \`return\` gives 3 values"

# An optional tuple result counts its tuple's values; `return n, n + 1`
# into `-> (int, int)?` is tests/regression/test_optional_tuple_return.ae.
cat > "$TMP/optional_count.ae" <<'AE'
pick(n: int) -> (int, int)? {
    if n > 0 { return n, n, n }
    return none
}

main() {
    println("${pick(1) == none}")
}
AE
refused optional_count 2:16 "'pick' returns (int, int)?, but this \`return\` gives 3 values"

if [ "$fails" -ne 0 ]; then
    echo "  [FAIL] return_type_check: $fails case(s)"
    exit 1
fi
echo "  [PASS] return_type_check: a returned value fits the result type"
