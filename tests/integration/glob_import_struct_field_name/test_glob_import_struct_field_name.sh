#!/bin/sh
# A field name in a struct literal is never a reference to a variable.
# lib/fontlib.ae has a private module-level `var size` beside
# `struct Font { face, size }`. The merge's rename of a module var's writes
# took the literal's `size: n` field initialiser (an AST_ASSIGNMENT naming the
# field) for a write to the var, so the C named a field `fontlib_size` the
# struct does not have: in the module's own make_font, and through the glob
# in the importer's `Font { size: 2 }`.
#
# Acceptance, main.ae prints exactly: 2 3 4 104 (104 = field 4 + 100 * the
# var, written once by make_font).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=glob_import_struct_field_name

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] $NAME: program errored (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
if [ "$got" != "2 3 4 104" ]; then
    echo "  [FAIL] $NAME: expected '2 3 4 104', got '$got'"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] $NAME"
exit 0
