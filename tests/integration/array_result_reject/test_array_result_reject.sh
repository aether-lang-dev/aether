#!/bin/sh
# Regression (#2886): no function can return a fixed-size array, alone or at
# a position of a tuple, and the type checker says so (E0201), pointing at a
# struct that holds the array.
#
# C returns no array and the code generator does not wrap one (making it a
# value a function returns is #2894), so every such result compiled to C
# that did not build: `int[2] pair(void);`, and a struct literal given the
# call (`Row { docs: two("w") }`) failed in `_aether_dup_each_Doc`. Each
# shape below is refused with E0201 and the help line naming the struct to
# write: a written result, one inferred from `return arr` or an array
# literal, a tuple with an array position (written and inferred), a closure,
# an extern, and a function type with such a result written as a parameter
# or a struct field. A struct holding the array, and a slice, are still
# returned, built and run.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
EXE=""
[ -x "$ROOT/build/ae" ] || EXE=".exe"
AE="$ROOT/build/ae$EXE"
AETHERC="$ROOT/build/aetherc$EXE"
if [ ! -x "$AE" ] || [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] array_result_reject: compiler not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

# refused <name> <expected message part> <expected array in the help>
refused() {
    name="$1"; want="$2"; arr="$3"
    if "$AETHERC" "$TMP/$name.ae" "$TMP/$name.c" > "$TMP/$name.log" 2>&1; then
        echo "  [FAIL] array_result_reject: $name compiled (should be refused)"
        fail=1
        return
    fi
    if ! grep -q "error\[E0201\]: $want" "$TMP/$name.log"; then
        echo "  [FAIL] array_result_reject: $name not refused with E0201 '$want'"
        grep -m3 "error" "$TMP/$name.log" | cut -c1-200 | sed 's/^/        /'
        fail=1
        return
    fi
    if ! grep -qF "help: wrap it in a struct and return the struct: struct Pair { v: $arr }" "$TMP/$name.log"; then
        echo "  [FAIL] array_result_reject: $name refused without the help naming $arr"
        grep -m3 "help" "$TMP/$name.log" | cut -c1-200 | sed 's/^/        /'
        fail=1
    fi
    if [ "$(grep -c 'error\[' "$TMP/$name.log")" -ne 1 ]; then
        echo "  [FAIL] array_result_reject: $name gives more than the one error"
        grep "error\[" "$TMP/$name.log" | cut -c1-200 | sed 's/^/        /'
        fail=1
    fi
}

cat > "$TMP/written.ae" <<'AE'
pair() -> int[2] {
    return [1, 2]
}

main() {
    a = pair()
    println("${a[0]}")
}
AE
refused written "'pair' returns int\[2\], a fixed-size array" "int[2]"

cat > "$TMP/inferred_local.ae" <<'AE'
pair() {
    int[2] a = [1, 2]
    return a
}

main() {
    a = pair()
    println("${a[0]}")
}
AE
refused inferred_local "'pair' returns int\[2\], a fixed-size array" "int[2]"

cat > "$TMP/inferred_literal.ae" <<'AE'
pair() {
    return [1, 2]
}

main() {
    a = pair()
    println("${a[0]}")
}
AE
refused inferred_literal "'pair' returns int\[2\], a fixed-size array" "int[2]"

cat > "$TMP/struct_elements.ae" <<'AE'
import std.string

struct Doc {
    name: string
    n: int
}

struct Row {
    docs: Doc[2]
}

two(w: string) -> Doc[2] {
    return [Doc { name: string.concat(w, "#0"), n: 0 }, Doc { name: string.concat(w, "#1"), n: 1 }]
}

main() {
    r = Row { docs: two("w") }
    println("${r.docs[0].name}")
}
AE
refused struct_elements "'two' returns Doc\[2\], a fixed-size array" "Doc[2]"

cat > "$TMP/tuple_written.ae" <<'AE'
pair() -> (int[2], int) {
    int[2] a = [1, 2]
    return a, 3
}

main() {
    a, n = pair()
    println("${a[0]} ${n}")
}
AE
refused tuple_written "'pair' returns (int\[2\], int), which holds the fixed-size array int\[2\]" "int[2]"

cat > "$TMP/tuple_inferred.ae" <<'AE'
pair() {
    int[2] a = [1, 2]
    return a, 3
}

main() {
    a, n = pair()
    println("${a[0]} ${n}")
}
AE
refused tuple_inferred "'pair' returns (int\[2\], int), which holds the fixed-size array int\[2\]" "int[2]"

cat > "$TMP/closure.ae" <<'AE'
main() {
    g = || {
        int[2] a = [1, 2]
        return a
    }
    a = g()
    println("${a[0]}")
}
AE
refused closure "this closure returns int\[2\], a fixed-size array" "int[2]"

cat > "$TMP/extern.ae" <<'AE'
extern pair_c() -> int[2]

main() {
    println("x")
}
AE
refused extern "extern 'pair_c' returns int\[2\], a fixed-size array" "int[2]"

cat > "$TMP/fn_param.ae" <<'AE'
run(g: fn() -> int[2]) -> int {
    return 0
}

main() {
    println("${run(null)}")
}
AE
refused fn_param "parameter 'g' holds the function type .* which returns int\[2\]" "int[2]"

cat > "$TMP/fn_field.ae" <<'AE'
struct Maker {
    factory: fn() -> int[3]
}

main() {
    println("x")
}
AE
refused fn_field "struct field 'factory' holds the function type .* which returns int\[3\]" "int[3]"

# What the help points to: a struct holding the array is returned, and so
# is a slice of the caller's storage.
cat > "$TMP/accepted.ae" <<'AE'
struct Pair {
    v: int[2]
}

pair() -> Pair {
    return Pair { v: [1, 2] }
}

tail(xs: int[]) -> int[] {
    return xs
}

main() {
    p = pair()
    int[3] arr = [7, 8, 9]
    t = tail(arr)
    println("${p.v[0]} ${p.v[1]} ${t[2]}")
}
AE
if ! ( cd "$TMP" && "$AE" build accepted.ae -o "$TMP/accepted" ) > "$TMP/accepted.log" 2>&1; then
    echo "  [FAIL] array_result_reject: a struct holding the array, or a slice, is not returned"
    grep -m5 "error" "$TMP/accepted.log" | cut -c1-200 | sed 's/^/        /'
    fail=1
else
    got="$("$TMP/accepted$EXE" 2>&1 | tr -d '\r')"
    if [ "$got" != "1 2 9" ]; then
        echo "  [FAIL] array_result_reject: the accepted program printed '$got', not '1 2 9'"
        fail=1
    fi
fi

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "  [PASS] array_result_reject: a fixed-size array result is refused (E0201) in ten shapes; a struct holding one, and a slice, are returned"
exit 0
