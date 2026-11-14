#!/bin/sh
# A float or a bool passed to a `ptr` parameter is a type error, and so is any
# scalar or string in a builder's `_ctx` slot.
#
# `f(p: ptr)` called as `f(0.1)` passed `ae check` and only the C compiler
# refused it ("passing 'double' to parameter of incompatible type 'void *'").
# It bit aether-ui's DSL: `text_color(_ctx: ptr, r, g, b)` called with four
# floats in a trailing block fills every parameter, so no `_ctx` is injected
# and `0.1` falls into it. The checker now says so at the argument, and for a
# `_ctx` first parameter hints that the block supplies it. Everything a `ptr`
# legitimately takes (an int, a string, null, other pointers, closures) still
# compiles and runs.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || AE="$ROOT/build/ae.exe"
[ -x "$AE" ] || { echo "  [SKIP] ptr_argument_reject: ae not built"; exit 0; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0
export AETHER_HOME="$ROOT"

expect_error() {  # file, fixed string, label
    out="$("$AE" check "$1" 2>&1 | tr -d '\r')"
    if ! printf '%s' "$out" | grep -qF "$2"; then
        echo "  [FAIL] ptr_argument_reject: $3"
        printf '%s\n' "$out" | head -6 | sed 's/^/        /'
        fail=1
    fi
}

cat > "$tmp/float.ae" <<'AE'
f(p: ptr) {
}

main() {
    f(0.1)
}
AE
expect_error "$tmp/float.ae" "Argument 1 'p' of 'f': expected ptr, got float (a float)" \
    "a float for a ptr parameter was accepted"

cat > "$tmp/bool.ae" <<'AE'
f(p: ptr) {
}

main() {
    f(true)
}
AE
expect_error "$tmp/bool.ae" "got bool (a bool)" "a bool for a ptr parameter was accepted"

# The DSL case: four floats and a trailing block fill every parameter.
cat > "$tmp/ctx.ae" <<'AE'
text_color(_ctx: ptr, r: float, g: float, b: float) {
}

panel(_ctx: ptr) {
}

main() {
    panel() {
        text_color(0.1, 0.2, 0.3, 0.4)
    }
}
AE
expect_error "$tmp/ctx.ae" "too many arguments? \`_ctx\` is supplied by the enclosing block" \
    "a _ctx given a float did not hint at the extra argument"

cat > "$tmp/ctx_str.ae" <<'AE'
label(_ctx: ptr, s: string) {
}

main() {
    label("a", "b")
}
AE
expect_error "$tmp/ctx_str.ae" "got string (a string)" "a string for _ctx was accepted"

cat > "$tmp/ctx_int.ae" <<'AE'
size(_ctx: ptr, w: int, h: int) {
}

main() {
    size(1, 2, 3)
}
AE
expect_error "$tmp/ctx_int.ae" "got int (an int)" "an int for _ctx was accepted"

# A `_ctx` the call leaves out shifts the arguments by one: the float here
# is the argument for `p`, two slots on. The check read it one slot further
# off still (the offset applied twice), so the float went through to C.
cat > "$tmp/ctx_shift.ae" <<'AE'
setp(_ctx: ptr, n: int, p: ptr) {
}

panel(_ctx: ptr) {
}

main() {
    panel() {
        setp(1, 0.5)
    }
}
AE
expect_error "$tmp/ctx_shift.ae" "Argument 2 'p' of 'setp': expected ptr, got float (a float)" \
    "a float after an injected _ctx was accepted"

# What a ptr takes stays working.
cat > "$tmp/ok.ae" <<'AE'
import std.list

show(p: ptr) -> int {
    if p == null {
        return 0
    }
    return 1
}

keep(p: @into ptr) -> ptr {
    return p
}


// A `_ctx` the call leaves out is not an argument, so a float after it is fine.
weigh(_ctx: ptr, name: string, w: float) {
}

main() {
    items = list.new()
    defer list.free(items)
    list.add(items, 7)
    println(show(null))
    println(show(items))
    println(show(keep(items)))
    println(show("text"))
    println(show(3))
    println(show(0))
    weigh("a", 0.5)
}
AE
out="$("$AE" run "$tmp/ok.ae" 2>&1 | tr -d '\r')"
want="$(printf '0\n1\n1\n1\n1\n0')"
if [ "$out" != "$want" ]; then
    echo "  [FAIL] ptr_argument_reject: legitimate ptr arguments did not run"
    printf '%s\n' "$out" | head -8 | sed 's/^/        /'
    fail=1
fi

[ "$fail" -eq 0 ] && echo "  [PASS] ptr_argument_reject"
exit "$fail"
