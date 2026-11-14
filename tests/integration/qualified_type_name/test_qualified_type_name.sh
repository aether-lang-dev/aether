#!/bin/sh
# Issue #946: a module-qualified type name `mod.Type` was accepted as a
# value/call (`lib.mk(...)`, #878) but NOT in a TYPE position — the parser
# stopped at the `.` with `Expected RIGHT_PAREN, got DOT` (param) /
# `Expected LEFT_BRACE, got DOT` (return). The bare exported name worked, so
# the qualifier — the disambiguator for two imports exporting the same type
# name — had no parseable spelling.
#
# Fixed in the type parser (qualified name in parse_type) plus the return-type
# disambiguator and the C-style-typed-local dispatcher. `mod.Type` resolves to
# the bare exported type (the merge brings it into the consumer unprefixed).
#
# Fixture (lib/lib.ae): struct Box + mk(). main.ae uses `lib.Box` as a
# parameter type, a return type, and a C-style typed local.
#
# The slice and array forms of the typed local, `lib.Box[] xs = []` and
# `lib.Box[2] pair = [...]`, went to the expression parser and stopped at
# the `[` (nib-code ask qualified-slice-local).
#
# Acceptance: compiles, runs, prints "6 7 2 8 7 3".

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] qualified_type_name: program errored (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
if [ "$got" != "6 7 2 8 7 3" ]; then
    echo "  [FAIL] qualified_type_name: expected '6 7 2 8 7 3', got '$got'"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] qualified_type_name: mod.Type accepted in param / return / typed-local positions, and as a slice or array local"
exit 0
