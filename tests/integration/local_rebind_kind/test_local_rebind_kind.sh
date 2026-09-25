#!/bin/sh
# A local has one type — the one its first binding gave it — and the
# unsigned 32-bit width is typed as C computes it.
#
# 1. `x = 5` then `x = "s"` (or `= true`, `= null`; `p = null` then
#    `p = 5`) replaced the symbol's type while the hoisted C variable took
#    one of the two, so the program failed in the C compiler ("assignment
#    to 'const char *' from 'int'"), nothing in the language's terms. It
#    is refused at the re-bind, naming both types. `s = null` into a
#    string and the numeric conversions stay legal.
# 2. `k = 5` then `k = <uint32>` kept `int k`: the #698 narrowing guard
#    now covers uint32 (values past 2^31 do not fit).
# 3. `uint32 + int` was typed int although the C computes it unsigned:
#    `y = u + 1` was `int y` and printed 4000000001 as -294967295. It is
#    uint32 now, as in C's usual arithmetic conversions; uint16/uint8
#    still promote to int.
# 4. `print("%d", b)` with a byte/uint8/uint16 argument warned "does not
#    match" although %d is exactly their conversion (they promote to
#    int); `%u` was not validated at all.
# 5. `foo` on a line of its own followed by `bar = 2` parsed as the
#    declaration `foo bar = 2` ("Type mismatch in variable
#    initialization" at `bar`); the binding name of an identifier-typed
#    declaration is on the type's line.
# 6. A legal re-bind keeps the FIRST binding's type instead of taking the
#    value's: `f = 1.5` then `f = u32` printed the double through `%u`
#    (0), `f = 2` went on as an int; `int x = 0` then `x = 2.5` narrows
#    as the annotation says.
# 7. A shift has the type of its promoted left operand: `-8 >> u32_count`
#    is -1, not 4294967295.
#
# A binding in a SIBLING block is deliberately not covered: it is a C
# variable of its own unless codegen hoists it (only when the name is
# used outside the branches), and std.cbor binds one name as int in one
# branch and long in another. The hoisted case is #2124.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] local_rebind_kind: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

expect_error() {  # file, pattern, label
    out="$(AETHER_HOME="$ROOT" "$AETHERC" "$1" "$tmp/out.c" 2>&1)"
    if ! printf '%s' "$out" | grep -q "$2"; then
        echo "  [FAIL] local_rebind_kind: $3"
        printf '%s\n' "$out" | grep -E "^error|-->|error:" | head -4 | sed 's/^/        /'
        fail=1
    fi
}

cat > "$tmp/str.ae" <<'AE'
main() {
    x = 5
    x = "s"
    println("${x}")
}
AE
expect_error "$tmp/str.ae" "cannot re-bind 'x' as string: it was bound as int by its first assignment" "int local re-bound to a string was not refused"

cat > "$tmp/bool.ae" <<'AE'
main() {
    x = 5
    x = true
    println("${x}")
}
AE
expect_error "$tmp/bool.ae" "cannot re-bind 'x' as bool: it was bound as int" "int local re-bound to a bool was not refused"

cat > "$tmp/null.ae" <<'AE'
main() {
    x = 5
    x = null
    println("${x}")
}
AE
expect_error "$tmp/null.ae" "cannot re-bind 'x' as ptr: it was bound as int" "int local re-bound to null was not refused"

cat > "$tmp/ptr.ae" <<'AE'
main() {
    p = null
    p = 5
    println("${p}")
}
AE
expect_error "$tmp/ptr.ae" "cannot re-bind 'p' as int: it was bound as ptr" "ptr local re-bound to an int was not refused"

cat > "$tmp/u32.ae" <<'AE'
main() {
    uint32 u = 4000000000
    k = 5
    k = u
    println("${k}")
}
AE
expect_error "$tmp/u32.ae" "narrowing assignment to 'k': its type was inferred as int from its initializer, but a uint32 is assigned" "uint32 into an inferred int local was not refused"

# The sibling-branch pattern std.cbor relies on keeps compiling: each
# branch's `v` is its own variable when nothing outside the branches
# uses the name.
cat > "$tmp/branches.ae" <<'AE'
pick(ai: int) -> long {
    if ai == 1 {
        v = 7
        return v as long
    }
    if ai == 2 {
        v = 4000000000
        return v
    }
    return 0
}
main() {
    println("${pick(1)} ${pick(2)}")
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/branches.ae" 2>&1 | tail -1)"
if [ "$got" != "7 4000000000" ]; then
    echo "  [FAIL] local_rebind_kind: sibling branches binding one name as int and long no longer compile (got '$got')"
    fail=1
fi

# A re-bind in an ENCLOSING scope is the same variable and is checked.
cat > "$tmp/nested.ae" <<'AE'
main() {
    v = "outer"
    if 2 > 0 { v = 5 }
    println("${v}")
}
AE
expect_error "$tmp/nested.ae" "cannot re-bind 'v' as int: it was bound as string" "a nested-block re-bind to another kind was not refused"

# A legal re-bind keeps the first binding's type.
cat > "$tmp/keep.ae" <<'AE'
main() {
    uint32 u = 7
    uint16 h = 3
    f = 1.5
    f = u
    g = 2.5
    g = h
    d = 1.5
    d = 2
    d = d / 4
    int x = 0
    x = 2.5
    uint32 bit = 3
    m = -8 >> bit
    n = 1 << bit
    println("${f} ${g} ${d} ${x} ${m} ${n}")
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/keep.ae" 2>&1 | tail -1)"
if [ "$got" != "7 3 0.5 2 -1 8" ]; then
    echo "  [FAIL] local_rebind_kind: a re-bound local did not keep its first type (got '$got')"
    fail=1
fi

# The re-binds that were always legal still are, and uint32 arithmetic
# is unsigned, printed unsigned, with the narrower kinds promoting to int.
cat > "$tmp/legal.ae" <<'AE'
extern malloc(n: int) -> ptr
extern free(p: ptr)

main() {
    p = null
    p = malloc(4)
    free(p)
    string s = "a"
    s = null
    t = "b"
    t = null
    n = 0
    n = 7
    f = 1.5
    f = 2
    uint32 u = 4000000000
    y = u + 1
    uint32 z = u * 2
    uint16 h = 65000
    hh = h + h
    byte b = 200
    bb = b + b               // byte op byte is a byte: 400 wraps to 144
    print("%d %d %u\n", h, b, u)
    println("${y} ${z} ${hh} ${bb} ${n} ${f} ${s == null} ${t == null}")
}
AE
want='65000 200 4000000000
4000000001 3705032704 130000 144 7 2 true true'
out="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/legal.ae" 2>&1)"
got="$(printf '%s\n' "$out" | tail -2)"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] local_rebind_kind: the legal program's output differs"
    printf '%s\n' "$out" | head -8 | sed 's/^/        /'
    fail=1
fi
if printf '%s' "$out" | grep -q "Format specifier"; then
    echo "  [FAIL] local_rebind_kind: %d with a byte/uint16 argument still warns"
    printf '%s\n' "$out" | grep "Format specifier" | head -2 | sed 's/^/        /'
    fail=1
fi
AETHER_HOME="$ROOT" "$AETHERC" "$tmp/legal.ae" "$tmp/legal.c" >/dev/null 2>&1
if ! grep -q "uint32_t y = " "$tmp/legal.c"; then
    echo "  [FAIL] local_rebind_kind: uint32 + int is not typed uint32 (expected 'uint32_t y = ' in the C)"
    fail=1
fi

# %u with a string is a mismatch and says so.
cat > "$tmp/fmt.ae" <<'AE'
main() {
    print("%u\n", "str")
}
AE
out="$(AETHER_HOME="$ROOT" "$AETHERC" "$tmp/fmt.ae" "$tmp/fmt.c" 2>&1)"
if ! printf '%s' "$out" | grep -q "Format specifier '%u' does not match argument type 'string'"; then
    echo "  [FAIL] local_rebind_kind: %u with a string argument is not diagnosed"
    fail=1
fi

# `_` is the discard binding: its type never sticks, so re-binding it to a
# different kind is legal (the whole point of `_` is to throw the value
# away). A named local in the same shape is refused; `_` is not.
cat > "$tmp/discard.ae" <<'AE'
extern malloc(n: int) -> ptr
extern free(p: ptr)

main() {
    _ = 5              // int
    p = malloc(4)
    _ = p              // ptr — a different kind, but `_` does not stick
    free(p)
    _ = "s"            // string
    println("ok")
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/discard.ae" 2>&1 | tail -1)"
if [ "$got" != "ok" ]; then
    echo "  [FAIL] local_rebind_kind: re-binding the discard '_' to another kind was refused (got '$got')"
    fail=1
fi

# The discard bound inside a LOOP body, then re-bound to another kind after
# it. The loop-hoisting pass used to declare `_` in the enclosing scope as
# int, so the string re-bind below failed with E0200 (aether-ui's vg hit it).
cat > "$tmp/discard_loop.ae" <<'AE'
ival() -> int { return 1 }
sval() -> string { return "s" }

drain() -> int {
    k = 0
    while k < 2 {
        _ = ival()
        k = k + 1
    }
    _ = sval()
    return k
}

main() {
    if drain() == 2 { println("ok") }
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/discard_loop.ae" 2>&1 | tail -1)"
if [ "$got" != "ok" ]; then
    echo "  [FAIL] local_rebind_kind: a discard '_' bound in a loop kept its type after the loop (got '$got')"
    fail=1
fi

# A bare expression statement, then a binding on the next line.
cat > "$tmp/lines.ae" <<'AE'
struct P { a: int }

main() {
    foo = 1
    foo
    bar = 2
    q = P { a: 3 }
    q.a
    baz = 4
    println("${foo} ${bar} ${baz}")
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/lines.ae" 2>&1 | tail -1)"
if [ "$got" != "1 2 4" ]; then
    echo "  [FAIL] local_rebind_kind: a bare expression statement swallowed the next line's binding (got '$got')"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] local_rebind_kind: a local keeps its first type; uint32 arithmetic and formats are unsigned; bare statements end at the line"
fi
exit $fail
