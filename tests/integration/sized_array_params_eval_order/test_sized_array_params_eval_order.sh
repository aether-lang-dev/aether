#!/bin/sh
# #2516: fixed-size array parameters and array-to-array assignment; a write
# to a const array's element is an Aether error; the evaluation-order gaps
# #2478 left.
#
# `f(xs: int[3])` and `a = b` between two `int[3]` locals reached the C
# compiler and failed there: a parameter now takes `E _param_xs[N]` and
# the body copies it into an array of its own (a value, as a struct is),
# and binding an array to another copies its elements. `TABLE[0] = v` on
# a const array was caught only by gcc. An assignment's own sides were
# unsequenced (`arr[i++] = i`), a closure literal was never ordered
# against the operands beside it, a call with named arguments was
# skipped, and what a C extern writes was not visible: an extern is opaque
# (#2524), evaluated ahead of a later operand that calls anything or reads
# through a pointer. Operands that read only plain locals stay inline.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] sized_array_params_eval_order: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"
fail=0

run_want() {  # file, want, label
    got="$("$AE" run "$1" 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
    if [ "$got" != "$2" ]; then
        echo "  [FAIL] sized_array_params_eval_order: $3"
        printf 'got:\n%s\nwant:\n%s\n' "$got" "$2" | sed 's/^/        /'
        fail=1
    fi
}

expect_error() {  # file, pattern, label
    out="$(AETHER_HOME="$ROOT" "$AETHERC" "$1" "$tmp/out.c" 2>&1)"
    if ! printf '%s' "$out" | grep -q "$2"; then
        echo "  [FAIL] sized_array_params_eval_order: $3"
        printf '%s\n' "$out" | grep -E "^error|-->|error:" | head -4 | sed 's/^/        /'
        fail=1
    fi
}

cat > "$tmp/arrays.ae" <<'AE'
import std.string

sum3(xs: int[3]) -> int { return xs[0] + xs[1] + xs[2] }

// The parameter is the callee's own copy: a write stays here, a closure's too.
bump(xs: int[3]) -> int {
    xs[0] = 100
    w = || { xs[2] = xs[2] + 1 }
    call(w)
    return sum3(xs)
}

join(ns: string[2]) -> string { return string.concat(ns[0], ns[1]) }

// Returned through another array.
copy_of(xs: int[3]) -> int {
    ys = [0, 0, 0]
    ys = xs
    ys[1] = 50
    return sum3(ys) - sum3(xs)
}

main() {
    a = [1, 2, 3]
    b = [7, 8, 9]
    println("sum ${sum3(a)} ${sum3(b)}")
    println("bump ${bump(a)} keeps ${a[0]} ${a[1]} ${a[2]}")
    a = b
    b[0] = 0
    println("assign ${a[0]} ${a[1]} ${a[2]} from ${b[0]}")
    names = ["ab", "cd"]
    println("strings ${join(names)}")
    println("copy ${copy_of(a)}")
}
AE
run_want "$tmp/arrays.ae" "sum 6 24
bump 106 keeps 1 2 3
assign 7 8 9 from 0
strings abcd
copy 42" "fixed-size array parameters and assignment"

printf 'sum3(xs: int[3]) -> int { return xs[0] + xs[1] + xs[2] }\nmain() {\n    b = [1, 2, 3, 4]\n    println("${sum3(b)}")\n}\n' > "$tmp/len.ae"
expect_error "$tmp/len.ae" "expected int\[3\], got int\[4\]; a fixed-size array parameter takes an array of exactly that length" "a longer array passed to an int[3] parameter was not refused"

printf 'main() {\n    a = [1, 2, 3]\n    b = [1, 2]\n    a = b\n    println("${a[0]}")\n}\n' > "$tmp/assign_len.ae"
expect_error "$tmp/assign_len.ae" "'a' is int\[3\] and cannot take int\[2\]" "binding an int[2] to an int[3] was not refused"

printf 'const TABLE[] = [1, 2, 3]\nmain() {\n    TABLE[0] = 5\n    println("${TABLE[0]}")\n}\n' > "$tmp/const.ae"
expect_error "$tmp/const.ae" "cannot write to an element of 'TABLE': it is a constant" "a write to a const array's element was not an Aether error"

printf 'const TABLE[] = [1, 2, 3]\nmain() {\n    TABLE[1]++\n    println("${TABLE[1]}")\n}\n' > "$tmp/const_inc.ae"
expect_error "$tmp/const_inc.ae" "cannot write to an element of 'TABLE': it is a constant" "a ++ on a const array's element was not an Aether error"

printf 'main() {\n    a = [1, 2, 3]\n    i = 0\n    a[i++] += 1\n    println("${a[0]}")\n}\n' > "$tmp/compound.ae"
expect_error "$tmp/compound.ae" "the target of a compound assignment is read and written, so a write inside it" "a write inside a compound assignment's target was not refused"

cat > "$tmp/order.ae" <<'AE'
import std.intmap

with(n: int, f: fn) -> int { return n * 10 + call(f) }
with_first(f: fn, n: int) -> int { return call(f) * 10 + n }
add3(a: int, b: int, c: int) -> int { return a * 100 + b * 10 + c }
pair(a: long, b: long) -> string { return "${a} ${b}" }
grow(m: ptr) -> long { return intmap.add(m, 2, 1) }

main() {
    arr = [0, 0, 0]
    i = 0
    arr[i++] = i
    j = 0
    brr = [7, 7]
    brr[j] = j++
    println("assign ${arr[0]} ${arr[1]} ${brr[0]} ${brr[1]} ${i} ${j}")
    k = 0
    println("closure ${with(k++, || { return k })} ${with_first(|| { return k }, k++)} ${k}")
    n = 0
    println("named ${add3(a: n++, b: n++, c: n)}")
    m = intmap.new(4)
    println("extern ${pair(intmap.get(m, 1, 0), intmap.add(m, 1, 5))} ${pair(intmap.get(m, 2, 0), grow(m))}")
    intmap.free(m)
}
AE
run_want "$tmp/order.ae" "assign 1 0 0 7 1 1
closure 1 11 2
named 12
extern 0 5 0 1" "an assignment's sides, a closure literal, named arguments and extern effects were not ordered"

# Operands that read only plain locals have nothing to order, beside a call
# or not: the C stays as it was (#2524).
cat > "$tmp/plain.ae" <<'AE'
import std.bytes

main() {
    b = bytes.new(4)
    bytes.set(b, 0, 1)
    k = 2
    v = bytes.get(b, 0) | k
    w = (k + 1) | (k * 2)
    println("${v} ${w}")
}
AE
if ! "$AETHERC" "$tmp/plain.ae" "$tmp/plain.c" >/dev/null 2>&1; then
    echo "  [FAIL] sized_array_params_eval_order: plain.ae did not compile"
    fail=1
elif grep -q "_eo[0-9]" "$tmp/plain.c"; then
    echo "  [FAIL] sized_array_params_eval_order: operands with nothing to order were hoisted"
    grep -n "_eo[0-9]" "$tmp/plain.c" | head -3 | sed 's/^/        /'
    fail=1
fi

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "  [PASS] sized_array_params_eval_order: fixed-size array parameters and assignment work, const element writes are refused, and assignments, closure literals, named arguments and extern effects are ordered"
