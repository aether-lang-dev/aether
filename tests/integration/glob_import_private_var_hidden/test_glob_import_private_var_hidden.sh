#!/bin/sh
# A glob import binds what the module exports, nothing else.
# lib/textlib.ae has private module-level `var`s `text` and `size`.
# `import textlib (*)` bound them in the importer anyway, so its loop
# variable `text` and its local `size` were the module's cells
# (`textlib_text = <string>` on a string[] in the C). A private var is not
# visible to the importer, and an importer's local always shadows; the
# same for a module that glob-imports it (lib/shout.ae).
#
# Acceptance, main.ae prints exactly: a b 7 0 10 1 d! 2

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=glob_import_private_var_hidden

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] $NAME: program errored (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
if [ "$got" != "a b 7 0 10 1 d! 2" ]; then
    echo "  [FAIL] $NAME: expected 'a b 7 0 10 1 d! 2', got '$got'"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] $NAME"
exit 0
