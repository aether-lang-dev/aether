#!/bin/sh
# Regression (#1644): `let a b = expr` must not compile.
#
# Two identifiers after `let` parsed as two declarations: an uninitialised one
# for the first name, and the real binding for the second. `let mut x = 0`
# (Rust habit) therefore compiled, and gave the program a stray variable named
# after a keyword Aether does not have. One test file in the tree had three.
#
# Also asserts the forms that legitimately follow `let` still compile, and that
# the rejection costs exactly one diagnostic: the parser drops the rest of the
# malformed statement rather than reporting its remains a second time.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] let_binds_nothing: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/bad.ae" <<'AEOF'
main() {
    let mut n = 0
    println("n=${n}")
}
AEOF

out="$("$AE" check "$TMP/bad.ae" 2>&1)"
if [ $? -eq 0 ]; then
    echo "  [FAIL] let_binds_nothing: \`let mut n = 0\` compiled"
    exit 1
fi
if ! printf '%s' "$out" | grep -q 'binds nothing'; then
    echo "  [FAIL] let_binds_nothing: rejected, but not for the stated reason:"
    printf '%s\n' "$out" | sed 's/^/        /' | head -10
    exit 1
fi

errors=$(printf '%s\n' "$out" | grep -c '^error')
if [ "$errors" -ne 1 ]; then
    echo "  [FAIL] let_binds_nothing: expected 1 diagnostic, got $errors"
    printf '%s\n' "$out" | sed 's/^/        /' | head -15
    exit 1
fi

# Everything that legitimately follows `let` / `var` still compiles.
cat > "$TMP/good.ae" <<'AEOF'
import std.string

struct Pt {
    x: int
}

pair() -> (int, string) {
    return 7, "seven"
}

main() {
    let x = 1
    let y: int = 5
    let s: string? = none
    let a, b = pair()
    @scoped let z = string.copy("scoped")
    var v = 3
    Pt p = Pt { x: 9 }
    println("${x} ${y} ${a} ${b} ${z} ${v} ${s ?? "none"} ${p.x}")
}
AEOF

if ! "$AE" check "$TMP/good.ae" >"$TMP/good.log" 2>&1; then
    echo "  [FAIL] let_binds_nothing: a valid \`let\` form stopped compiling"
    sed 's/^/        /' "$TMP/good.log" | head -10
    exit 1
fi

echo "  [PASS] let_binds_nothing: an unbound \`let\` is one clear error; valid forms unaffected"
