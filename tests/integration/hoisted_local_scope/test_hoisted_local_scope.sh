#!/bin/sh
# Regression (#2186): a local first bound in an `if` arm or a `while` body
# is readable after the block in every function, not only in main, and
# exactly where codegen hoists it.
#
# Codegen hoists such a local to an enclosing scope by three rules, now
# defined once in compiler/analysis/hoist.c and used by both codegen and the
# typechecker: the arm locals of a function body's top-level ifs that a
# top-level statement reads (hoist_if_branch_vars), the names both arms of an
# if/else declare (hoist_if_else_common_vars), and a while body's locals,
# nested ones included (hoist_loop_vars). The typechecker gave every block
# its own scope, so the read after it resolved only through names the early
# inference pass had left in
# the program table -- which it did for main, whose locals it never popped,
# and never for any other function:
#
#     show(c: int) { if c == 1 { heading = "A" } else { heading = "B" }
#                    println(heading) }       // Undefined variable 'heading'
#
# The typechecker now binds those names in the scope codegen hoists them to,
# so main needs no leftovers and inference pops main's locals like any
# function's (which is what stopped a local of main shadowing module
# functions and externs, #2173).
set -eu

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
AE="$ROOT/build/ae"
if [ ! -x "$AE" ]; then
    echo "  [SKIP] hoisted_local_scope: build/ae not built"
    exit 0
fi
AETHER_HOME=""
export AETHER_HOME

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail() {
    echo "  [FAIL] hoisted_local_scope: $1"
    exit 1
}

cat > "$WORK/fn.ae" <<'AE'
extern exit(code: int)

pick(c: int) -> string {
    if c == 1 {
        heading = "A"
    } else {
        heading = "B"
    }
    println("${heading}")
    return heading
}

// A while body's locals, including one bound in an if nested in it.
count_to(n: int) -> int {
    i = 0
    while i < n {
        last = i
        if i == 1 {
            seen_one = 1
        }
        i = i + 1
    }
    return last + seen_one
}

// An if/else nested in a while body: both arms bind the name, so it is
// declared before the loop and readable inside it after the if.
labels() -> string {
    out = ""
    i = 0
    while i < 3 {
        if i == 0 { label = "a" } else { label = "b" }
        out = "${out}${label}"
        i = i + 1
    }
    return out
}

// Bound in both arms with different numeric kinds: the read after the if
// sees the joined type, as codegen's hoisted declaration has it.
widen(c: int) -> float {
    if c == 1 {
        v = 2
    } else {
        v = 2.5
    }
    return v
}

main() {
    if pick(1) != "A" || pick(2) != "B" { println("FAIL pick"); exit(1) }
    if count_to(3) != 3 { println("FAIL count_to: ${count_to(3)}"); exit(1) }
    if widen(2) != 2.5 { println("FAIL widen: ${widen(2)}"); exit(1) }
    if labels() != "abb" { println("FAIL labels: ${labels()}"); exit(1) }
    println("PASS")
}
AE
out="$("$AE" run "$WORK/fn.ae" 2>&1)" || { echo "$out" | sed 's/^/    /' | head -20; fail "function-level branch and loop locals did not build or run"; }
[ "$(echo "$out" | tail -1)" = "PASS" ] || { echo "$out" | sed 's/^/    /'; fail "expected PASS"; }

# Where codegen does NOT hoist -- an if nested inside another block -- the
# local stays scoped to its arm, and reading it after is the typechecker's
# error, not a C compiler's.
cat > "$WORK/nested.ae" <<'AE'
show(c: int) {
    if c == 1 {
        if c > 0 {
            deep = "x"
        }
    }
    println(deep)
}

main() {
    show(1)
}
AE
if "$AE" build "$WORK/nested.ae" -o "$WORK/nested" > "$WORK/nested.log" 2>&1; then
    fail "a local bound in a nested if was readable after it, where codegen does not hoist it"
fi
grep -q "Undefined variable 'deep'" "$WORK/nested.log" \
    || { sed 's/^/    /' "$WORK/nested.log" | head -10; fail "expected the typechecker's Undefined variable 'deep'"; }

# An arm local read only inside a later if's body is not hoisted by codegen
# (no top-level statement reads it), so the read is the typechecker's error
# too -- not a C compiler's "'x' undeclared".
cat > "$WORK/later_if.ae" <<'AE'
f(a: int, b: int) {
    if a == 1 { x = 5 }
    if b == 1 { println("${x}") }
}

main() {
    f(1, 1)
}
AE
if "$AE" build "$WORK/later_if.ae" -o "$WORK/later_if" > "$WORK/later_if.log" 2>&1; then
    fail "a read the C cannot make compiled"
fi
grep -q "Undefined variable 'x'" "$WORK/later_if.log" \
    || { sed 's/^/    /' "$WORK/later_if.log" | head -10; fail "expected the typechecker's Undefined variable 'x', not a C error"; }

echo "  [PASS] hoisted_local_scope: branch and loop locals readable after their block wherever codegen hoists them"
