#!/bin/sh
# #2591: `&f()` is a type error that says what to do. A call's result is not
# stored anywhere, so it has no address; it used to pass the type checker
# and reach the C compiler as "lvalue required as unary '&' operand". The
# same value bound to a local first builds and runs.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
EXE="${EXE_EXT:-}"
if [ -z "$EXE" ] && [ ! -x "$ROOT/build/ae" ] && [ -x "$ROOT/build/ae.exe" ]; then
    EXE=".exe"
fi
AE="$ROOT/build/ae$EXE"
if [ ! -x "$AE" ]; then
    echo "  [SKIP] address_of_call: build/ae not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/bad.ae" <<'AE'
struct Pair {
    a: int
    b: int
}
make_pair() -> Pair { return Pair { a: 1, b: 2 } }
sum(p: *Pair) -> int { return p.a + p.b }
main() {
    println("${sum(&make_pair())}")
}
AE
if "$AE" build "$tmp/bad.ae" -o "$tmp/bad" >"$tmp/bad.log" 2>&1; then
    echo "  [FAIL] address_of_call: \`&make_pair()\` built"
    exit 1
fi
if ! grep -q "bind it to a local first" "$tmp/bad.log" || grep -q "lvalue required" "$tmp/bad.log"; then
    echo "  [FAIL] address_of_call: no type error naming the fix:"
    sed 's/^/    /' "$tmp/bad.log" | head -8
    exit 1
fi

cat > "$tmp/good.ae" <<'AE'
struct Pair {
    a: int
    b: int
}
make_pair() -> Pair { return Pair { a: 1, b: 2 } }
sum(p: *Pair) -> int { return p.a + p.b }
main() {
    p = make_pair()
    println("${sum(&p)}")
}
AE
got="$("$AE" run "$tmp/good.ae" 2>&1 | tail -1)"
if [ "$got" != "3" ]; then
    echo "  [FAIL] address_of_call: the local form printed '$got', want 3"
    exit 1
fi

echo "  [PASS] address_of_call: \`&f()\` is a type error naming the fix, and the local form runs"
exit 0
