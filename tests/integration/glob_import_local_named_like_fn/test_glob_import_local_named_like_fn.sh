#!/bin/sh
# A local of the importer named like a glob-imported function.
# `button = button(t)` declares a local `button` that hides the function
# from its binding on (docs/language-reference.md, #2683); the call on its
# own right-hand side comes before the binding, so it is the module's
# function, and the checker resolves it so. The dead-code prune took every
# call spelt like a local of the function for a call through that local and
# dropped buttons_button, so the C called an undeclared function.
#
# A call after the binding is through the local, which is no function: the
# checker took it for the import instead and the C called the dropped
# function. It is E0301 now, as for a program's own function, naming the
# local and the imported function.
#
# Acceptance:
#   1. main.ae prints exactly: 1 2
#   2. shadowed.ae fails to build with E0301 naming 'button' and
#      'buttons.button', not with a C compiler error.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=glob_import_local_named_like_fn

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] $NAME: main.ae errored (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
if [ "$got" != "1 2" ]; then
    echo "  [FAIL] $NAME: expected '1 2', got '$got'"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run shadowed.ae 2>&1 )
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "  [FAIL] $NAME: shadowed.ae built; a call through the local must be E0301"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi
if ! printf '%s\n' "$out" | grep -q "E0301.*'button' is a local here, which hides the imported function 'buttons.button'"; then
    echo "  [FAIL] $NAME: shadowed.ae did not fail with the E0301 naming both"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] $NAME"
exit 0
