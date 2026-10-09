#!/bin/sh
# A closure stored in a typed function pointer slot is refused by the type
# checker in every position, not only as an argument (#2628).
#
# A typed fn pointer (`fn(ptr) -> string`, a `cfn` name) is a bare C
# function pointer; a closure is an `_AeClosure`, a function and its
# environment, so one can never be stored as the other. The argument
# position was already E0200. A struct literal's field, a field store
# (through a pointer too), an element store, a declaration's annotation, a
# re-bind of such a local, a module-level `var` assigned in a function and
# a function's `-> Getter` result passed `ae check`, and gcc reported
# "incompatible types ... using type '_AeClosure'" against the generated C.
# Now each is E0200 naming the slot, for a closure literal and for a name
# bound to one. A named function in those slots is its address and keeps
# working, and a bare `fn` field or variable keeps taking closures.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] closure_fnptr_slot: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

expect_error() {  # file, fixed string, label
    out="$(cd "$tmp" && AETHER_HOME="$ROOT" "$AE" check "$1" 2>&1)"
    if ! printf '%s' "$out" | grep -qF "$2"; then
        echo "  [FAIL] closure_fnptr_slot: $3"
        printf '%s\n' "$out" | grep -E "^error|-->" | head -4 | sed 's/^/        /'
        fail=1
    fi
}

FIELD="a closure cannot be stored in a typed function pointer; the field is a C function pointer"
VAR="a closure cannot be bound to a typed function pointer"

# The issue's shape: closure literals in a struct literal's fields.
cat > "$tmp/literal.ae" <<'AE'
struct Field {
    get_text: fn(ptr) -> string
    set_text: fn(ptr, string)
}

main() {
    f = Field { get_text: |p: ptr| { return "lit" }, set_text: |p: ptr, s: string| { println(s) } }
    g = f.get_text
    println(g(null))
}
AE
expect_error literal.ae "Field 'Field.get_text': $FIELD" \
    "a closure literal in a struct literal's fn-pointer field was not refused"
expect_error literal.ae "Field 'Field.set_text': $FIELD" \
    "a second closure field in the same literal was not refused"

# A name bound to a closure, into a struct literal's field.
cat > "$tmp/literal_name.ae" <<'AE'
struct Field {
    get_text: fn(ptr) -> string
}

main() {
    cl = |p: ptr| { return "lit" }
    f = Field { get_text: cl }
    println(f.get_text(null))
}
AE
expect_error literal_name.ae "Field 'Field.get_text': $FIELD" \
    "a closure local in a struct literal's fn-pointer field was not refused"

# Field stores: on a value, through a pointer, and of a closure local.
cat > "$tmp/store.ae" <<'AE'
struct Field {
    get_text: fn(ptr) -> string
}

name_of(p: ptr) -> string { return "named" }

reset(pf: *Field) {
    pf.get_text = |p: ptr| { return "through" }
}

main() {
    f = Field { get_text: name_of }
    f.get_text = |p: ptr| { return "lit" }
    reset(&f)
    println(f.get_text(null))
}
AE
expect_error store.ae "store.ae:13:18" \
    "a closure stored in a fn-pointer field was not refused"
expect_error store.ae "store.ae:8:19" \
    "a closure stored in a fn-pointer field through a pointer was not refused"
expect_error store.ae "Field 'Field.get_text': $FIELD" \
    "the field store message does not name the field"

cat > "$tmp/store_name.ae" <<'AE'
struct Field {
    get_text: fn(ptr) -> string
}

name_of(p: ptr) -> string { return "named" }

main() {
    f = Field { get_text: name_of }
    cl = |p: ptr| { return "lit" }
    f.get_text = cl
    println(f.get_text(null))
}
AE
expect_error store_name.ae "Field 'Field.get_text': $FIELD" \
    "a closure local stored in a fn-pointer field was not refused"

# Bindings: an annotated local, a closure local into one, a re-bind, and a
# module-level `var` assigned in a function and initialized.
cat > "$tmp/annotated.ae" <<'AE'
main() {
    let g: fn(ptr) -> string = |p: ptr| { return "lit" }
    println(g(null))
}
AE
expect_error annotated.ae "Variable 'g': $VAR" \
    "a closure bound to an annotated fn-pointer local was not refused"

cat > "$tmp/annotated_name.ae" <<'AE'
main() {
    cl = |p: ptr| { return "lit" }
    let g: fn(ptr) -> string = cl
    println(g(null))
}
AE
expect_error annotated_name.ae "Variable 'g': $VAR" \
    "a closure local bound to an annotated fn-pointer local was not refused"

cat > "$tmp/rebind.ae" <<'AE'
name_of(p: ptr) -> string { return "named" }

main() {
    let g: fn(ptr) -> string = name_of
    g = |p: ptr| { return "lit" }
    println(g(null))
}
AE
expect_error rebind.ae "Variable 'g': $VAR" \
    "a closure re-bound to a fn-pointer local was not refused"

cat > "$tmp/global.ae" <<'AE'
name_of(p: ptr) -> string { return "named" }

var g: fn(ptr) -> string = name_of

main() {
    g = |p: ptr| { return "lit" }
    println(g(null))
}
AE
expect_error global.ae "Variable 'g': $VAR" \
    "a closure assigned to a module-level fn-pointer var was not refused"

cat > "$tmp/global_init.ae" <<'AE'
var g: fn(ptr) -> string = |p: ptr| { return "lit" }

main() {
    println(g(null))
}
AE
expect_error global_init.ae "Variable 'g': $VAR" \
    "a closure initializing a module-level fn-pointer var was not refused"

# A `cfn` name is the same slot: a field, a local and a function's result.
cat > "$tmp/cfn.ae" <<'AE'
cfn Getter(ptr) -> string

struct Field {
    get_text: Getter
}

getter() -> Getter {
    return |p: ptr| { return "lit" }
}

main() {
    f = Field { get_text: |p: ptr| { return "lit" } }
    let g: Getter = |p: ptr| { return "lit" }
    println(f.get_text(null))
    println(g(null))
}
AE
expect_error cfn.ae "Field 'Field.get_text': $FIELD" \
    "a closure in a cfn-typed field was not refused"
expect_error cfn.ae "Variable 'g': $VAR" \
    "a closure bound to a cfn-typed local was not refused"
expect_error cfn.ae "Return value: a closure cannot be returned as a typed function pointer" \
    "a closure returned as a cfn result was not refused"

# An element of an array of fn pointers.
cat > "$tmp/element.ae" <<'AE'
name_of(p: ptr) -> string { return "named" }

main() {
    xs = [name_of as fn(ptr) -> string]
    xs[0] = |p: ptr| { return "lit" }
    g = xs[0]
    println(g(null))
}
AE
expect_error element.ae "Element of 'xs': a closure cannot be stored in a typed function pointer" \
    "a closure stored in a fn-pointer array element was not refused"

# What stays legal, built and run: a named function in every typed slot
# (its address), and closures in a bare `fn` field and variable.
cat > "$tmp/ok.ae" <<'AE'
cfn Getter(ptr) -> string

struct Field {
    get_text: fn(ptr) -> string
    set_text: fn(ptr, string)
}

struct Hooks {
    on_text: Getter
}

struct Holder {
    cb: fn
}

var g_text: fn(ptr) -> string = name_of

name_of(p: ptr) -> string { return "named" }
other_name(p: ptr) -> string { return "other" }
show(p: ptr, s: string) { println("set ${s}") }

getter() -> Getter {
    return other_name
}

reset(pf: *Field) {
    pf.get_text = other_name
}

main() {
    f = Field { get_text: name_of, set_text: show }
    println(f.get_text(null))
    f.set_text(null, "x")
    f.get_text = other_name
    println(f.get_text(null))
    f.get_text = name_of
    reset(&f)
    println(f.get_text(null))
    h = Hooks { on_text: getter() }
    println(h.on_text(null))
    let g: fn(ptr) -> string = name_of
    g = other_name
    println(g(null))
    g_text = other_name
    println(g_text(null))

    base = 40
    hold = Holder { cb: |x: int| { return x + base } }
    println(call(hold.cb, 2))
    hold.cb = |x: int| { return x * base }
    println(call(hold.cb, 2))
    let c: fn = |x: int| { return x - base }
    println(call(c, 50))
    c = |x: int| { return x + 1 }
    println(call(c, 50))
}
AE
got="$(cd "$tmp" && AETHER_HOME="$ROOT" "$AE" run ok.ae 2>&1 | tr -d '\r' | tail -11 | tr '\n' ' ')"
if [ "$got" != "named set x other other other other other 42 80 10 51 " ]; then
    echo "  [FAIL] closure_fnptr_slot: a named function or a bare fn closure was refused or misread (got '$got')"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] closure_fnptr_slot: a closure in a typed fn-pointer field, element, binding or result is refused; named functions and bare fn closures pass"
fi
exit $fail
