#!/bin/sh
# A local first bound inside a branch or loop body and used after it is
# ONE variable for the whole function (#2124).
#
# Codegen hoists such a local to function scope. Two things were wrong
# with that: (1) a binding of another kind in a sibling branch or loop
# body reached the C compiler ("assignment to 'const char *' from
# 'int'"), and (2) the hoisted declaration took the FIRST branch's type
# while the early inference pass recorded the LAST binding's, so
# `if a { f = 1.5 } else { f = 2 }` then `${f}` printed 1 through %d, and
# `n = 1` / `n = 4000000000` in two branches truncated the long.
#
# Now: a hoisted local is declared with the numeric join of every binding
# of the name in the function (float over int, 64-bit over 32-bit, uint32
# over int) and the early pass records the same join; a binding whose
# kind cannot flow into the hoisted type is refused in the language's
# terms at that binding. A name bound in sibling branches and used only
# inside them is not hoisted and keeps a variable per branch (std.cbor's
# `read_value` binds one name as int and long that way).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] hoisted_local_rebind: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

expect_error() {  # file, pattern, label
    out="$(AETHER_HOME="$ROOT" "$AETHERC" "$1" "$tmp/out.c" 2>&1)"
    if ! printf '%s' "$out" | grep -q "$2"; then
        echo "  [FAIL] hoisted_local_rebind: $3"
        printf '%s\n' "$out" | grep -E "^error|-->|error:" | head -4 | sed 's/^/        /'
        fail=1
    fi
}

cat > "$tmp/branch.ae" <<'AE'
main() {
    if 1 > 0 { v = "outer" }
    if 2 > 0 { v = 5 }
    println("${v}")
}
AE
expect_error "$tmp/branch.ae" "cannot bind 'v' as int: it is bound as string in another branch or loop body of this function" "a sibling-branch binding of another kind was not refused"

cat > "$tmp/loop.ae" <<'AE'
main() {
    i = 0
    while i < 1 { w = 1.5; i = i + 1 }
    for k in 0..2 { w = "x" }
    println("${w}")
}
AE
expect_error "$tmp/loop.ae" "cannot bind 'w' as float: it is bound as string in another branch or loop body of this function" "a sibling-loop binding of another kind was not refused"

cat > "$tmp/nullnum.ae" <<'AE'
main() {
    if 1 > 0 { p = 5 }
    if 2 > 0 { p = null }
    println("${p}")
}
AE
expect_error "$tmp/nullnum.ae" "cannot bind 'p' as ptr: it is bound as int in another branch" "null into a number local bound in a sibling branch was not refused"

# The join: every binding fits the hoisted variable, and a use after the
# branches sees the joined type.
cat > "$tmp/join.ae" <<'AE'
pick(flag: int) -> long {
    if flag == 1 { n = 1 } else { n = 4000000000 }
    if flag == 2 { n = 7 }
    return n
}

main() {
    if 3 > 0 { f = 1.5 } else { f = 2 }
    if 2 > 0 { m = 4000000000 } else { m = 1 }
    uint32 u = 4000000000
    if 1 > 0 { x = 1 } else { x = u }
    i = 0
    while i < 1 { g = 2; i = i + 1 }
    while i < 2 { g = 2.5; i = i + 1 }
    if 1 > 0 { s = "a" }
    if 2 > 0 { s = null }
    if 1 > 0 { t = null }
    if 2 > 0 { t = "b" }
    println("${pick(1)} ${pick(0)} ${pick(2)} ${f} ${m} ${x} ${g} ${s == null} ${t}")
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/join.ae" 2>&1 | tail -1)"
if [ "$got" != "1 4000000000 7 1.5 4000000000 1 2.5 true b" ]; then
    echo "  [FAIL] hoisted_local_rebind: joined hoisted locals print wrong (got '$got')"
    fail=1
fi
AETHER_HOME="$ROOT" "$AETHERC" "$tmp/join.ae" "$tmp/join.c" >/dev/null 2>&1
for decl in 'int64_t n = 0;' 'double f = 0;' 'int64_t m = 0;' 'uint32_t x = 0;' 'double g = 0;'; do
    if ! grep -q "$decl" "$tmp/join.c"; then
        echo "  [FAIL] hoisted_local_rebind: expected the joined declaration '$decl' in the C"
        fail=1
    fi
done

# Not hoisted (used only inside its branches): one variable per branch,
# each with its own numeric type (std.cbor's read_value shape).
cat > "$tmp/local.ae" <<'AE'
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
main() { println("${pick(1)} ${pick(2)}") }
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/local.ae" 2>&1 | tail -1)"
if [ "$got" != "7 4000000000" ]; then
    echo "  [FAIL] hoisted_local_rebind: branch-local int/long bindings of one name no longer compile independently (got '$got')"
    fail=1
fi

# A string binding hoists the name for the whole function (its heap
# tracker lives at function scope), so an int binding of the same name
# in another branch never compiled (`const char* v` then `v = 7`); it is
# refused with the same message rather than by the C compiler.
cat > "$tmp/localstr.ae" <<'AE'
pick(ai: int) -> long {
    if ai == 1 {
        v = 7
        return v as long
    }
    if ai == 2 {
        v = "seven"
        println(v)
        return 0
    }
    return 0
}
main() { println("${pick(1)} ${pick(2)}") }
AE
expect_error "$tmp/localstr.ae" "cannot bind 'v' as int: it is bound as string in another branch" "an int binding beside a string binding of the same branch-local name was not refused"

# A distinct-string value into a string local: the same kind, one C
# variable, no diagnostic (std.language's README binds `tag = raw as
# language.Tag`; every string local is hoisted with its heap tracker).
cat > "$tmp/distinct.ae" <<'AE'
type Tag = distinct string

main() {
    raw = "en-US"
    tag = raw as Tag
    if 1 > 0 { s = "x" }
    if 2 > 0 { s = "y" as Tag }
    println("${tag as string} ${s as string}")
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/distinct.ae" 2>&1 | tail -1)"
if [ "$got" != "en-US y" ]; then
    echo "  [FAIL] hoisted_local_rebind: a distinct string into a string local was refused or misprinted (got '$got')"
    fail=1
fi

# A name reused across functions is unrelated: `src` an int local in one
# function and a `ptr` parameter in the next (std.cryptography.sha3's
# shape) — the join is per body, not per name.
cat > "$tmp/reuse.ae" <<'AE'
keccak() -> int {
    src = 3
    xs = [ 10, 20, 30, 40 ]
    return xs[src]
}
absorb(src: ptr, off: int) -> int { return off }
main() { println("${keccak()} ${absorb(null, 5)}") }
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/reuse.ae" 2>&1 | tail -1)"
if [ "$got" != "40 5" ]; then
    echo "  [FAIL] hoisted_local_rebind: a name reused with another kind in another function broke (got '$got')"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] hoisted_local_rebind: a hoisted local takes the join of its bindings; a binding of another kind is refused"
fi
exit $fail
