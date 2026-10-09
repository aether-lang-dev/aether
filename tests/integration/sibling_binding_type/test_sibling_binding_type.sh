#!/bin/sh
# A local hoisted out of a branch or loop body is one variable, so a binding
# of it in a sibling block with another struct or pointer type is refused by
# the type checker (#2611).
#
# `view = block as *Apple` in one while body and `view = block as *Pear` in
# the next passed `ae check`, and the C declared one `Apple* view` for both
# (gcc: "assignment to 'Apple *' from incompatible pointer type 'Pear *'").
# The same for structs A and B bound to one name in the arms of an if/else.
# Codegen's sibling check refuses a binding of another kind (an int beside a
# string) and leaves bindings of one kind to the type checker's nominal
# rules, which handed sibling bindings back to codegen unjudged; and the
# checker's hoisted local was untyped when the early inference pass could not
# type its first binding (an `as *T` view). Now the first binding checked
# types it, and a sibling binding of another struct or pointer type is the
# E0200 an int beside a string is. A re-bind in one block names both pointer
# types instead of calling each `ptr`. Bindings used only inside separate
# ifs keep a variable each, and a bare `ptr`, `null` or a @c_struct overlay
# pointer (a `void*` in C) beside a typed pointer stays legal.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] sibling_binding_type: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

expect_error() {  # file, fixed string, label
    out="$(AETHER_HOME="$ROOT" "$AE" check "$1" 2>&1)"
    if ! printf '%s' "$out" | grep -qF "$2"; then
        echo "  [FAIL] sibling_binding_type: $3"
        printf '%s\n' "$out" | grep -E "^error|-->" | head -4 | sed 's/^/        /'
        fail=1
    fi
}

# The issue's shape: one name, two pointer views, two sibling loops.
cat > "$tmp/loops.ae" <<'AE'
extern calloc(count: int, size: int) -> ptr
extern free(p: ptr)

struct Apple {
    seeds: int
}

struct Pear {
    stem: int
    weight: int
}

main() {
    block = calloc(1, 64)
    i = 0
    while i < 2 {
        view = block as *Apple
        view.seeds = 3
        i = i + 1
    }
    i = 0
    while i < 2 {
        view = block as *Pear
        view.weight = 7
        i = i + 1
    }
    free(block)
}
AE
expect_error "$tmp/loops.ae" "cannot bind 'view' as *Pear: it is bound as *Apple in another branch or loop body of this function" \
    "a pointer to another struct bound in a sibling loop was not refused"

# Two structs in the arms of an if/else.
cat > "$tmp/arms.ae" <<'AE'
struct A { x: int }
struct B { y: float, z: int }

pick(k: int) -> int {
    total = 0
    if k == 0 {
        d = A { x: 3 }
        total = d.x
    } else {
        d = B { y: 1.5, z: 4 }
        total = d.z
    }
    return total
}

main() { println("${pick(0)} ${pick(1)}") }
AE
expect_error "$tmp/arms.ae" "cannot bind 'd' as B: it is bound as A in another branch or loop body of this function" \
    "another struct bound in the else arm was not refused"

# One block: the message names both pointer types.
cat > "$tmp/one.ae" <<'AE'
extern calloc(count: int, size: int) -> ptr
struct Apple { seeds: int }
struct Pear { stem: int, weight: int }
main() {
    block = calloc(1, 64)
    view = block as *Apple
    view.seeds = 1
    view = block as *Pear
}
AE
expect_error "$tmp/one.ae" "cannot re-bind 'view' as *Pear: it was bound as *Apple by its first assignment" \
    "the re-bind message does not name the two pointer types"

# What stays legal: the same pointer type in sibling loops, `null` beside a
# typed pointer, an overlay pointer beside a typed one, and structs of two
# types bound to one name used only inside separate ifs.
cat > "$tmp/ok.ae" <<'AE'
extern calloc(count: int, size: int) -> ptr
extern free(p: ptr)

struct Apple { seeds: int }
struct Pear { stem: int, weight: int }

@c_struct Hdr {
    a: uint32 @0
}

pick(k: int, block: ptr) -> int {
    total = 0
    if k == 0 {
        one = block as *Pear
        total = one.weight
    }
    if k == 1 {
        one = block as *Apple
        total = one.seeds
    }
    return total
}

main() {
    block = calloc(1, 64)
    i = 0
    while i < 2 {
        view = block as *Apple
        view.seeds = view.seeds + 1
        i = i + 1
    }
    while i < 4 {
        view = block as *Apple
        view.seeds = view.seeds + 10
        i = i + 1
    }
    while i < 5 {
        held = block as *Apple
        i = i + 1
    }
    while i < 6 {
        held = null
        i = i + 1
    }
    while i < 7 {
        hdr = block as *Hdr
        i = i + 1
    }
    while i < 8 {
        hdr = block as *Apple
        i = i + 1
    }
    (block as *Pear).weight = 9
    println("${pick(1, block)} ${pick(0, block)} ${held == null}")
    free(block)
}
AE
got="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/ok.ae" 2>&1 | tail -1)"
if [ "$got" != "22 9 true" ]; then
    echo "  [FAIL] sibling_binding_type: a legal sibling binding was refused or misread (got '$got')"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] sibling_binding_type: a sibling binding of another struct or pointer type is refused; same-type, null, overlay and separate-if bindings pass"
fi
exit $fail
