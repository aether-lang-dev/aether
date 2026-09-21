#!/bin/sh
# `x op= rhs` is checked as `x = x op rhs`.
#
# The compound-assignment statement typechecked only its right-hand side:
# no operator check (`s += "b"` on strings reached the C compiler as an
# invalid pointer addition), no result check, and none of the narrowing
# guards a plain `=` has — `x = 0` then `x += 2.5` stored 2, `y = 1` then
# `y *= 4000000000` stored -294967296, `z = 5` then `z += <uint32>` went
# negative, all silently. Each is now the same diagnostic the `=` form
# gives; annotated slots still narrow on purpose and the legal forms are
# untouched.
#
# A compound assignment to a FIELD or an ELEMENT (`p.n += 2`,
# `xs[i] <<= 2`) did not parse at all ("Expected statement in block");
# it is `p.n = p.n + 2` and is built as that. A target the language
# cannot evaluate twice (one with a call in it) is refused with a message
# that says so.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] compound_assignment_types: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

expect_error() {  # file, pattern, label
    out="$(AETHER_HOME="$ROOT" "$AETHERC" "$1" "$tmp/out.c" 2>&1)"
    if ! printf '%s' "$out" | grep -q "$2"; then
        echo "  [FAIL] compound_assignment_types: $3"
        printf '%s\n' "$out" | grep -E "^error|-->|error:" | head -4 | sed 's/^/        /'
        fail=1
    fi
}

printf 'main() {\n    x = 0\n    x += 2.5\n    println("${x}")\n}\n' > "$tmp/f.ae"
expect_error "$tmp/f.ae" "narrowing assignment to 'x': its type was inferred as int from its initializer, but a float is assigned" "float += into an inferred int was not refused"

printf 'main() {\n    y = 1\n    y *= 4000000000\n    println("${y}")\n}\n' > "$tmp/l.ae"
expect_error "$tmp/l.ae" "narrowing assignment to 'y': its type was inferred as 32-bit int from its initializer, but a 64-bit value is assigned" "64-bit *= into an inferred int was not refused"

printf 'main() {\n    uint32 u = 4000000000\n    z = 5\n    z += u\n    println("${z}")\n}\n' > "$tmp/u.ae"
expect_error "$tmp/u.ae" "narrowing assignment to 'z': its type was inferred as int from its initializer, but a uint32 is assigned" "uint32 += into an inferred int was not refused"

printf 'main() {\n    s = "a"\n    s += "b"\n    println(s)\n}\n' > "$tmp/s.ae"
expect_error "$tmp/s.ae" "'+=' is not defined for strings, use" "+= on strings was not refused in the language's terms"

printf 'main() {\n    b = true\n    b += 1\n    println("${b}")\n}\n' > "$tmp/b.ae"
expect_error "$tmp/b.ae" "'+=' is not defined for bool and int" "+= between bool and int was not refused"

cat > "$tmp/legal.ae" <<'AE'
main() {
    f = 1.5
    f += 2
    long n = 1
    n *= 4000000000
    int k = 0
    k += 2.5
    i = 1
    i <<= 3
    i |= 4
    i -= 2
    i %= 7
    uint32 u = 4000000000
    u += 1
    byte c = 250
    c += 3
    println("${f} ${n} ${k} ${i} ${u} ${c}")
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/legal.ae" 2>&1 | tail -1)"
if [ "$got" != "3.5 4000000000 2 3 4000000001 253" ]; then
    echo "  [FAIL] compound_assignment_types: legal compound assignments changed (got '$got')"
    fail=1
fi

cat > "$tmp/fields.ae" <<'AE'
struct P { n: int, f: float }
struct Q { p: P }
main() {
    p = P { n: 1, f: 1.0 }
    p.n += 2
    p.f += 2
    p.n *= 3
    xs = [ 1, 2 ]
    xs[0] += 4
    i = 1
    xs[i - 1] <<= 2
    ys = [ 1.5, 2.5 ]
    ys[1] += 2
    q = Q { p: P { n: 5, f: 0.5 } }
    q.p.n -= 1
    q.p.f *= 4
    println("${p.n} ${p.f} ${xs[0]} ${ys[1]} ${q.p.n} ${q.p.f}")
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/fields.ae" 2>&1 | tail -1)"
if [ "$got" != "9 3 20 4.5 4 2" ]; then
    echo "  [FAIL] compound_assignment_types: compound assignment to fields and elements (got '$got')"
    fail=1
fi

# The element guard applies through the compound form too.
printf 'main() {\n    xs = [ 1, 2 ]\n    xs[0] += 2.5\n    println("${xs[0]}")\n}\n' > "$tmp/elem.ae"
expect_error "$tmp/elem.ae" "narrowing assignment to an element of 'xs'" "float += into an inferred int array element was not refused"

printf 'f() -> int { return 0 }\nmain() {\n    xs = [ 1, 2 ]\n    xs[f()] += 1\n    println("${xs[0]}")\n}\n' > "$tmp/call.ae"
out="$(AETHER_HOME="$ROOT" "$AETHERC" "$tmp/call.ae" "$tmp/out.c" 2>&1)"
if ! printf '%s' "$out" | grep -q "the target of a compound assignment must be a variable, a field or an element with no call in it"; then
    echo "  [FAIL] compound_assignment_types: a call inside the target was not refused with the message"
    fail=1
fi
if [ "$(printf '%s\n' "$out" | grep -c '^error')" != "1" ]; then
    echo "  [FAIL] compound_assignment_types: the refusal should be the only error reported"
    printf '%s\n' "$out" | grep '^error' | sed 's/^/        /'
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] compound_assignment_types: x op= rhs is checked as x = x op rhs, on variables, fields and elements"
fi
exit $fail
