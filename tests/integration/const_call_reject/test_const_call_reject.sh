#!/bin/sh
# Tests that `const X = some_function_call()` is rejected at
# typecheck time. Per Nico's design call: const inlines its RHS at
# every use site, so `const G = make_thing()` would silently re-call
# the function on every reference. Reject these at compile time with
# a helpful diagnostic that points the user at std.config /
# std.actors for process-global state. Section 2 of
# nuther-ask-of-aether-team.md.
#
# Exception (issue #482, compile-time constant evaluation phase-1):
# a HARD-WHITELISTED set of pure conversions — string.from_int /
# string.from_long / string.from_float / string.concat — IS admissible
# in a const initializer because the optimizer folds them to a literal
# at compile time (no re-evaluation at use sites). Every OTHER call
# stays rejected. See docs/compile-time-eval.md.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
# A directory of its own, not fixed /tmp paths: two sweeps at once (two
# checkouts, or parallel agents) overwrote each other's files (#2770).
AE_TMP="$(mktemp -d)"
trap 'rm -rf "$AE_TMP"' EXIT

pass=0
fail=0

expect_reject() {
    label="$1"
    file="$2"
    out=$(AETHER_HOME="" "$ROOT/build/ae" build "$file" -o $AE_TMP/ae_const_reject_out 2>&1)
    if echo "$out" | grep -q "const initializer must be a compile-time constant expression"; then
        echo "  [PASS] $label"
        pass=$((pass + 1))
    else
        echo "  [FAIL] $label — expected reject; got:"
        echo "$out" | head -8 | sed 's/^/    /'
        fail=$((fail + 1))
    fi
}

expect_accept() {
    label="$1"
    file="$2"
    if AETHER_HOME="" "$ROOT/build/ae" build "$file" -o $AE_TMP/ae_const_ok_out 2>/dev/null; then
        echo "  [PASS] $label"
        pass=$((pass + 1))
    else
        echo "  [FAIL] $label — expected accept"
        fail=$((fail + 1))
    fi
}

# Case 1: top-level const = function call (the headline trap).
cat > $AE_TMP/ae_const_bad1.ae << 'EOF'
extern malloc(size: int) -> ptr
const G_BUF = malloc(64)
main() { println("ok") }
EOF
expect_reject "top-level const = function call" $AE_TMP/ae_const_bad1.ae

# Case 2: top-level const = NON-whitelisted std-namespaced call.
# string.length is a real pure stdlib function but it is NOT on the
# compile-time-fold whitelist, so it must still be rejected.
cat > $AE_TMP/ae_const_bad2.ae << 'EOF'
import std.string
const N = string.length("hi")
main() { println("ok") }
EOF
expect_reject "top-level const = non-whitelisted std call" $AE_TMP/ae_const_bad2.ae

# Case 2b (issue #482): the whitelisted pure conversions ARE accepted
# and fold to a literal — string.from_int / from_long / from_float /
# concat. These do not re-evaluate at use sites because the optimizer
# replaces the call with the literal it produces.
cat > $AE_TMP/ae_const_ok_fold.ae << 'EOF'
import std.string
const ID = string.from_int(42)
const VER = string.concat("v", "1")
main() { println("ok") }
EOF
expect_accept "whitelisted from_int/concat const folds (issue #482)" $AE_TMP/ae_const_ok_fold.ae

# Case 3: function-local const = call.
cat > $AE_TMP/ae_const_bad3.ae << 'EOF'
extern malloc(size: int) -> ptr
main() {
    const LOCAL = malloc(64)
    println("ok")
}
EOF
expect_reject "function-local const = call" $AE_TMP/ae_const_bad3.ae

# Case 4: legit literal const compiles.
cat > $AE_TMP/ae_const_ok1.ae << 'EOF'
const PI = 3.14
const MAX = 100
const NAME = "alice"
main() { println("ok") }
EOF
expect_accept "literal const RHS still compiles" $AE_TMP/ae_const_ok1.ae

# Case 5: arithmetic on literals compiles.
cat > $AE_TMP/ae_const_ok2.ae << 'EOF'
const MAX = 100
const HALF = MAX / 2
const DOUBLE = MAX * 2
main() { println("ok") }
EOF
expect_accept "arithmetic on const RHS still compiles" $AE_TMP/ae_const_ok2.ae

# Case 6: a struct literal of constants is a constant, so a const array of
# them compiles; one field that is a call keeps it rejected.
cat > $AE_TMP/ae_const_ok3.ae << 'EOF2'
struct XY { x: int, y: int }
const PTS[] = [XY { x: 1, y: 2 }, XY { x: 3, y: -4 }]
const ORIGIN = XY { x: 0, y: 0 }
main() { println("ok") }
EOF2
expect_accept "const array / const of constant struct literals compile" $AE_TMP/ae_const_ok3.ae

cat > $AE_TMP/ae_const_bad4.ae << 'EOF2'
struct XY { x: int, y: int }
f() -> int { return 1 }
const PTS[] = [XY { x: 1, y: 2 }, XY { x: f(), y: 4 }]
main() { println("ok") }
EOF2
expect_reject "const array of struct literal with a call field" $AE_TMP/ae_const_bad4.ae

cat > $AE_TMP/ae_const_bad5.ae << 'EOF2'
struct XY { x: int, y: int }
f() -> int { return 1 }
const ORIGIN = XY { x: 0, y: f() }
main() { println("ok") }
EOF2
expect_reject "const struct literal with a call field" $AE_TMP/ae_const_bad5.ae

# Case 7: an array literal as a plain `const` (naeb's ask
# aether-array-literal-return-and-const.md). The array form is spelt with
# brackets on the name, `const NAMES[] = [...]`; the error said the
# initialiser had to be a compile-time constant and talked about function
# calls, which the literal is not, and never named the form that works.
cat > $AE_TMP/ae_const_bad6.ae << 'EOF2'
const NAMES = ["a", "b"]
main() { print(NAMES[0]) }
EOF2
out=$(AETHER_HOME="" "$ROOT/build/ae" build $AE_TMP/ae_const_bad6.ae -o $AE_TMP/ae_const_reject_out 2>&1)
if echo "$out" | grep -q "E0200" &&
   echo "$out" | grep -qF 'const NAMES[] = [...]' &&
   ! echo "$out" | grep -q "function calls are inlined"; then
    echo "  [PASS] const = array literal names the const NAMES[] form"
    pass=$((pass + 1))
else
    echo "  [FAIL] const = array literal: expected E0200 naming \`const NAMES[] = [...]\`; got:"
    echo "$out" | head -8 | sed 's/^/    /'
    fail=$((fail + 1))
fi
cat > $AE_TMP/ae_const_ok4.ae << 'EOF2'
const NAMES[] = ["a", "b"]
names() -> string[] { return NAMES }
main() { print(names()[1]) }
EOF2
expect_accept "the form the message names compiles, and a function may return it" $AE_TMP/ae_const_ok4.ae

rm -f $AE_TMP/ae_const_bad*.ae $AE_TMP/ae_const_ok*.ae \
      $AE_TMP/ae_const_reject_out $AE_TMP/ae_const_ok_out $AE_TMP/ae_const_ok_fold.ae

echo ""
echo "const_call_reject: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
