#!/bin/sh
# A typed pointer or a struct is not a string, and a string is not a typed
# pointer. Argument checking let `f(s: string)` take `vc: *Vcr` with no
# diagnostic (a binding and a `return` already refused it): the C compiler
# then saw a `Vcr*` given to a `const char*`, an error under gcc 14 but only
# a warning under clang, so on macOS the callee read the struct's bytes as
# characters. Found when a servirtium-vcr refactor turned ~165 functions'
# string key into `vc: *Vcr` and some call sites kept passing the old one.
# A bare `ptr` stays the universal pointer both ways, and `null` is still a
# null pointer.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || AE="$ROOT/build/ae.exe"
[ -x "$AE" ] || { echo "  [SKIP] string_pointer_argument_reject: ae not built"; exit 0; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

# Each refused call, alone in a program, and the error it must give.
refuse() {
    name="$1"; want="$2"
    out="$(AETHER_HOME="$ROOT" "$AE" check "$tmp/$name.ae" 2>&1)"
    if ! printf '%s' "$out" | grep -qF "$want"; then
        echo "  [FAIL] string_pointer_argument_reject ($name): not refused with: $want"
        printf '%s\n' "$out" | grep -E "^error" | head -3 | sed 's/^/        /'
        fail=1
    fi
}

cat > "$tmp/ptr_to_string.ae" <<'AE'
struct Vcr { name: string; n: int }
takes(key: string) -> int { return 0 }
main() {
    vc = heap.new(Vcr)
    println("${takes(vc)}")
}
AE
refuse ptr_to_string "Argument 1 'key' of 'takes': expected string, got *Vcr"

cat > "$tmp/param_to_string.ae" <<'AE'
struct Vcr { name: string; n: int }
takes(key: string, n: int) -> int { return n }
caller(vc: *Vcr) -> int { return takes(vc, 1) }
main() {
    println("${caller(heap.new(Vcr))}")
}
AE
refuse param_to_string "Argument 1 'key' of 'takes': expected string, got *Vcr"

cat > "$tmp/extern_string.ae" <<'AE'
struct Vcr { name: string; n: int }
extern strlen(s: string) -> int
main() {
    vc = heap.new(Vcr)
    println("${strlen(vc)}")
}
AE
refuse extern_string "Argument 1 's' of 'strlen': expected string, got *Vcr"

cat > "$tmp/struct_to_string.ae" <<'AE'
struct Vcr { name: string; n: int }
takes(key: string) -> int { return 0 }
main() {
    v = Vcr { name: "x", n: 1 }
    println("${takes(v)}")
}
AE
refuse struct_to_string "Argument 1 'key' of 'takes': expected string, got Vcr"

cat > "$tmp/string_to_ptr.ae" <<'AE'
struct Vcr { name: string; n: int }
takes(vc: *Vcr) -> int { return 0 }
main() {
    println("${takes("tape-name")}")
}
AE
refuse string_to_ptr "Argument 1 'vc' of 'takes': expected *Vcr, got string"

# A bare `ptr` into an Aether `string` parameter is refused too (E0202, the
# rule a `string` local follows): `vc as ptr` no longer launders the struct
# into the string (nib-code ask ptr-passed-as-string).
cat > "$tmp/ptr_to_string.ae" <<'AE'
struct Vcr { name: string; n: int }
takes(key: string) -> int { return 0 }
main() {
    vc = heap.new(Vcr)
    p = vc as ptr
    println("${takes(p)}")
}
AE
out="$(AETHER_HOME="$ROOT" "$AE" check "$tmp/ptr_to_string.ae" 2>&1)" && {
    echo "  [FAIL] string_pointer_argument_reject: a bare ptr into a string parameter was accepted"
    exit 1
}
case "$out" in
    *"error[E0202]: Argument 1 'key' of 'takes': expected string, got ptr"*) ;;
    *) echo "  [FAIL] string_pointer_argument_reject: ptr_to_string refused without E0202:"
       printf '%s\n' "$out" | sed 's/^/        /'
       exit 1 ;;
esac

# What stays accepted: a string into a bare ptr, null to a typed pointer, a
# typed pointer into a bare ptr, and the string field the caller meant.
cat > "$tmp/ok.ae" <<'AE'
struct Vcr { name: string; n: int }
takes(key: string) -> int { return 0 }
takes_ptr(p: ptr) -> int { return 0 }
takes_vcr(vc: *Vcr) -> int { return 0 }
main() {
    vc = heap.new(Vcr)
    vc.name = "tape"
    a = takes(vc.name)
    b = takes_ptr("text")
    c = takes_vcr(null)
    d = takes_ptr(vc)
    println("${a + b + c + d}")
}
AE
out="$(AETHER_HOME="$ROOT" "$AE" check "$tmp/ok.ae" 2>&1)" || {
    echo "  [FAIL] string_pointer_argument_reject: a legitimate call was refused"
    printf '%s\n' "$out" | grep -E "^error" | head -3 | sed 's/^/        /'
    fail=1
}

[ "$fail" = 0 ] || exit 1
echo "  [PASS] string_pointer_argument_reject"
