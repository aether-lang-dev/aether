#!/bin/sh
# Regression: `none` cannot be bound as a name, and the error says so where the
# name is bound (#2018).
#
# #340 made `none` the empty-optional literal: any bare identifier spelt `none`
# in expression position IS the literal. A local, parameter or constant with
# that name could be declared, but never read -- every use parsed as the
# literal -- and the failure arrived two concepts away as "cannot interpolate
# this value" or "invalid operation for given types", or in one shape as
# malformed C from codegen.
#
# Each case below used to parse. Each must now stop at the declaration, with a
# diagnostic that names the collision and points at the binding, not at a use.
# The last case is the other half of the contract: the literal itself, and a
# `match` arm spelt `none`, still work exactly as before.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
AE="$ROOT/build/ae"

if [ ! -x "$AETHERC" ] && [ ! -x "$AETHERC.exe" ]; then
    echo "  [SKIP] none_is_not_a_name: $AETHERC not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fail=0

# Compile one program, expect it to be refused, and expect the refusal to land
# on the given line and mention the binding role. The line matters: a
# diagnostic at the use would be the bug this test exists to catch.
refuse() {
    name="$1"; line="$2"; role="$3"
    out="$("$AETHERC" "$tmp/$name.ae" "$tmp/$name.c" 2>&1)"
    if ! printf '%s' "$out" | grep -q "'none' is the empty-optional literal and cannot be used as a $role"; then
        echo "  [FAIL] none_is_not_a_name: $name was not refused as a $role"
        printf '%s\n' "$out" | sed 's/^/        /' | head -6
        fail=1
        return
    fi
    if ! printf '%s' "$out" | grep -q "$name.ae:$line:"; then
        echo "  [FAIL] none_is_not_a_name: $name refused, but not at line $line (the declaration)"
        printf '%s\n' "$out" | sed 's/^/        /' | head -6
        fail=1
        return
    fi
    if [ -s "$tmp/$name.c" ]; then
        echo "  [FAIL] none_is_not_a_name: $name still reached codegen"
        fail=1
    fi
}

# (a) a local, then interpolated -- the issue's first repro.
cat > "$tmp/local.ae" <<'AE'
main() {
    none = 5
    println("${none}")
}
AE
refuse local 2 "variable name"

# (b) a local in arithmetic -- the issue's second repro.
cat > "$tmp/arith.ae" <<'AE'
pick() -> int {
    none = 0
    x = none + 1
    return x
}
main() { println("${pick()}") }
AE
refuse arith 2 "variable name"

# (c) a function parameter.
cat > "$tmp/param.ae" <<'AE'
pick(none: int) -> int {
    return none + 1
}
main() { println("${pick(1)}") }
AE
refuse param 1 "parameter name"

# (d) a closure parameter.
cat > "$tmp/closure.ae" <<'AE'
main() {
    f = | none: int | { return none + 1 }
    println("${f(1)}")
}
AE
refuse closure 2 "parameter name"

# (e) a typed declaration -- the shape that reached codegen as
#     `const char* none = NULL;`.
cat > "$tmp/typed.ae" <<'AE'
main() {
    string none = "x"
    println(none)
}
AE
refuse typed 2 "variable name"

# (f) a constant.
cat > "$tmp/constant.ae" <<'AE'
const none = 3
main() { println("${none}") }
AE
refuse constant 1 "constant name"

# (g) one slot of a tuple destructure.
cat > "$tmp/destructure.ae" <<'AE'
pair() -> (int, int) { return (1, 2) }
main() {
    a, none = pair()
    println("${a}")
}
AE
refuse destructure 3 "variable name"

# (h) the literal and the match arm are untouched. This is the other half of
#     the fix: refusing the name must not have cost the language the value.
cat > "$tmp/literal.ae" <<'AE'
describe(x: int?) -> int {
    let r: int = match x {
        none -> -1
        some(v) -> v
    }
    return r
}
main() {
    let missing: int? = none
    println("${describe(missing)} ${describe(4)}")
}
AE
got="$("$AE" run "$tmp/literal.ae" 2>&1 | tail -1)"
if [ "$got" != "-1 4" ]; then
    echo "  [FAIL] none_is_not_a_name: the none literal and match arm no longer work (got '$got', want '-1 4')"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] none_is_not_a_name: seven binding positions refused at the declaration, the literal and match arm untouched"
fi
exit $fail
