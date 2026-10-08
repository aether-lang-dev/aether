#!/bin/sh
# A string literal in a function-clause pattern is matched by content (#2467).
#
# The clause dispatcher wrote the pattern's value raw: `greet("bob")` became
# `if (_arg0 == bob)`, an undeclared name, and even quoted it would have
# compared pointers. It is now a quoted, escaped C string literal compared
# with string_equals, NULL-safe, the way a `match` string arm compares. The
# argument below is built at run time for one call, so a pointer comparison
# could not pass it.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] function_clause_string_pattern: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

cat > "$tmp/main.ae" <<'AE'
greet("bob") -> "hi bob!"
greet("say \"hi\"") -> "quoted"
greet(name) -> "hello ${name}"

kind(0, "x") -> 1
kind(n, "y") -> 2
kind(n, s) -> 3

main() {
    println(greet("bob"))
    println(greet("amy"))
    println(greet("say \"hi\""))
    b = "b"
    println(greet("bo${b}"))
    println("${kind(0, "x")} ${kind(5, "y")} ${kind(0, "z")}")
}
AE

want='hi bob!
hello amy
quoted
hi bob!
1 2 3'
got="$("$AE" run "$tmp/main.ae" 2>&1 | tr -d '\r' | grep -v "^warning\|^ *-->\|^ *[0-9]* |\|^ *|\|^Type checking\|^$")"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] function_clause_string_pattern: wrong output"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] function_clause_string_pattern: string patterns in function clauses compare by content"
