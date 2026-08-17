#!/bin/sh
# Issue #1573: `import mod (fn)` where `fn` reads its module's private `var`.
#
# The selective-import merge filtered module-level declarations by the caller's
# selection list, which is right for the import SURFACE (functions, consts) and
# wrong for module STATE: a `var` (#701) is private, so it can never appear in a
# selection list, yet every selected function that touches it carries a renamed
# reference to it. The cell was left behind and typechecking failed with
# "Undefined variable '<ns>_<var>'". Whole-module import was the workaround.
#
# Acceptance: main.ae prints 11, 12, 12 — the writes reach one shared cell
# across three separately-merged functions.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] selective_import_module_var: $AE not built"; exit 0; }

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] selective_import_module_var: program errored (rc=$rc)"
    printf '%s\n' "$out" | head -12 | sed 's/^/          /'
    exit 1
fi

got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
want="11 12 12"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] selective_import_module_var: got '$got', want '$want'"
    exit 1
fi

echo "  [PASS] selective_import_module_var: the private cell came along and is shared"
exit 0
