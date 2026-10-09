#!/bin/sh
# A typed pointer passed to a parameter of another pointer type is a type
# error at the call (#2624).
#
# `buffer_size(b: *Buffer)` given `&p.ints`, an `*Ints`, passed `ae check`:
# argument checking for user functions compared struct values (#2491) but
# never pointers, so gcc reported "incompatible pointer type" against the
# generated C, and with two structs sharing a layout prefix nothing would
# have failed and the callee would have read the wrong struct. Now the call
# is E0200 naming both pointer types, for `&local`, `&p.field`, `&p.a.b` and
# a typed pointer local, across a module boundary or not. The pointer rules
# of an assignment apply: a bare `ptr` parameter takes any pointer, a
# `*Buffer` reached through `lib.Buffer` is the same type, and a pointer to a
# @c_struct overlay (a `void*` in C) takes any pointer.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] pointer_argument_type: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

mkdir -p "$tmp/lib"
cat > "$tmp/lib/module.ae" <<'AE'
exports (Ints, Buffer, Holder, Outer, ints_size, buffer_size, raw_size, new_buffer)
struct Ints { data: ptr, capacity: int }
struct Buffer { data: ptr, capacity: int, stride: int }
struct Holder { ints: Ints, buffer: Buffer }
struct Outer { holder: Holder }
ints_size(a: *Ints) -> int { return a.capacity * 4 }
buffer_size(b: *Buffer) -> int { return b.capacity * b.stride }
raw_size(b: ptr) -> int { return (b as *Buffer).capacity }
new_buffer(block: ptr) -> *Buffer {
    b = block as *Buffer
    b.capacity = 5
    b.stride = 2
    return b
}
AE

expect_error() {  # file, fixed string, label
    out="$(cd "$tmp" && AETHER_HOME="$ROOT" "$AE" check "$1" 2>&1)"
    if ! printf '%s' "$out" | grep -qF "$2"; then
        echo "  [FAIL] pointer_argument_type: $3"
        printf '%s\n' "$out" | grep -E "^error|-->" | head -4 | sed 's/^/        /'
        fail=1
    fi
}

# The issue's shape: the address of a field, through a pointer, into a
# module function taking another struct's pointer.
cat > "$tmp/field.ae" <<'AE'
import lib
holder_of(block: ptr) -> *Holder { return block as *Holder }
main() {
    h = Holder { ints: Ints { data: null, capacity: 3 }, buffer: Buffer { data: null, capacity: 2, stride: 8 } }
    p = holder_of((&h) as ptr)
    println("${lib.buffer_size(&p.ints)}")
}
AE
expect_error field.ae "Argument 1 'b' of 'lib.buffer_size': expected *Buffer, got *Ints" \
    "&p.field of another struct type was not refused at the call"

cat > "$tmp/local.ae" <<'AE'
import lib
main() {
    ints = Ints { data: null, capacity: 1 }
    println("${lib.buffer_size(&ints)}")
}
AE
expect_error local.ae "Argument 1 'b' of 'lib.buffer_size': expected *Buffer, got *Ints" \
    "&local of another struct type was not refused at the call"

cat > "$tmp/nested.ae" <<'AE'
import lib
size_of(b: *Buffer) -> int { return b.capacity }
main() {
    o = Outer { holder: Holder { ints: Ints { data: null, capacity: 3 }, buffer: Buffer { data: null, capacity: 2, stride: 8 } } }
    q = (&o) as ptr
    oo = q as *Outer
    println("${size_of(&oo.holder.ints)}")
}
AE
expect_error nested.ae "Argument 1 'b' of 'size_of': expected *Buffer, got *Ints" \
    "&p.a.b of another struct type was not refused at a same-module call"

cat > "$tmp/typed.ae" <<'AE'
import lib
size_of(b: *Buffer) -> int { return b.capacity }
main() {
    ints = Ints { data: null, capacity: 1 }
    view = (&ints) as ptr as *Ints
    println("${size_of(view)}")
}
AE
expect_error typed.ae "Argument 1 'b' of 'size_of': expected *Buffer, got *Ints" \
    "a typed pointer local of another struct type was not refused"

# What stays legal, built and run: same-type pointers, a bare `ptr`
# parameter, a pointer made from `lib.Buffer`, a pointer the module
# returned, a module's own calls, and a @c_struct overlay parameter.
cat > "$tmp/ok.ae" <<'AE'
import lib
extern calloc(count: int, size: int) -> ptr
extern free(p: ptr)
@c_struct Hdr {
    a: uint32 @0
}
holder_of(block: ptr) -> *Holder { return block as *Holder }
takes_buffer(b: *Buffer) -> int { return lib.buffer_size(b) }
takes_raw(b: ptr) -> int { return lib.raw_size(b) }
by_value(x: lib.Buffer) -> int { return lib.buffer_size(&x) + takes_buffer(&x) }
read_hdr(h: *Hdr) -> int { return 1 }
main() {
    h = Holder { ints: Ints { data: null, capacity: 3 }, buffer: Buffer { data: null, capacity: 2, stride: 8 } }
    o = Outer { holder: h }
    p = holder_of((&h) as ptr)
    q = (&o) as ptr
    oo = q as *Outer
    b2 = Buffer { data: null, capacity: 4, stride: 4 }
    block = calloc(1, 64)
    nb = lib.new_buffer(block)
    println("${lib.buffer_size(&p.buffer)} ${lib.ints_size(&p.ints)} ${lib.buffer_size(&oo.holder.buffer)} ${lib.buffer_size(&b2)} ${lib.raw_size(&p.buffer)} ${takes_raw(&b2)} ${by_value(b2)} ${takes_buffer(nb)} ${read_hdr(&b2)}")
    free(block)
}
AE
got="$(cd "$tmp" && AETHER_HOME="$ROOT" "$AE" run ok.ae 2>&1 | tail -1)"
if [ "$got" != "16 12 16 16 2 4 32 10 1" ]; then
    echo "  [FAIL] pointer_argument_type: a legal pointer argument was refused or misread (got '$got')"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] pointer_argument_type: a pointer to another type is refused at the call; ptr, same-type and overlay arguments pass"
fi
exit $fail
