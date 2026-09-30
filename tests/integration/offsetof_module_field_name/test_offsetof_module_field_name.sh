#!/bin/sh
# #2320: offsetof(T, f) inside a module keeps `f` a member name.
#
# The module merge renames a module's references to its own functions and
# constants to their prefixed names. It walked into offsetof's field operand
# too, so `offsetof(Thing, strength)` in a module with a `strength` getter
# became `offsetof(struct Thing, shade_strength)`: "'struct Thing' has no
# member named 'shade_strength'". The field operand is a member name and is
# never renamed now, whether a function or a constant shares it.
#
# Acceptance: compiles, runs, prints "true", "true", "true", "8".

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] offsetof_module_field_name: program errored (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
if [ "$got" != "true true true 8" ]; then
    echo "  [FAIL] offsetof_module_field_name: expected 'true true true 8', got '$got'"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] offsetof_module_field_name: a field named like a module function or const"
exit 0
