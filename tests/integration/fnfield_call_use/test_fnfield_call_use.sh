#!/bin/sh
# A call through a local's fn-pointer field is a use of the local (#2635).
#
# `f = Field { get_text: name_of }` then `println(f.get_text(null))` warned
# W1001 "unused variable 'f'". The parser collapses `f.get_text(...)` to a
# call named `f.get_text` and drops the receiver, so the usage walk, which
# looks for identifiers and for calls named after a local, never saw `f`.
# The checker tags such a call (#749), and the walk now takes the receiver
# from it, by value and through a pointer. A local that really is unused
# still warns.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] fnfield_call_use: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

cat > "$tmp/use.ae" <<'AE'
struct Field {
    get_text: fn(ptr) -> string
    count: fn(int) -> int
}

name_of(p: ptr) -> string { return "named" }
twice(n: int) -> int { return n * 2 }

main() {
    f = Field { get_text: name_of, count: twice }
    println(f.get_text(null))
    g = Field { get_text: name_of, count: twice }
    pg = &g
    println(pg.count(21))
    idle = Field { get_text: name_of, count: twice }
}
AE

out="$(cd "$tmp" && AETHER_HOME="$ROOT" "$AE" check use.ae 2>&1)"
for v in f pg; do
    if printf '%s' "$out" | grep -qF "unused variable '$v'"; then
        echo "  [FAIL] fnfield_call_use: '$v' used only through a fn-pointer field call was reported unused"
        fail=1
    fi
done
if ! printf '%s' "$out" | grep -qF "unused variable 'idle'"; then
    echo "  [FAIL] fnfield_call_use: a local that is never used was not reported"
    fail=1
fi

got="$(cd "$tmp" && AETHER_HOME="$ROOT" "$AE" run use.ae 2>&1 | tr -d '\r' | tail -2 | tr '\n' ' ')"
if [ "$got" != "named 42 " ]; then
    echo "  [FAIL] fnfield_call_use: the calls through the fields printed '$got'"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "  [PASS] fnfield_call_use: a call through a local's fn-pointer field uses the local; an unused one still warns"
fi
exit $fail
