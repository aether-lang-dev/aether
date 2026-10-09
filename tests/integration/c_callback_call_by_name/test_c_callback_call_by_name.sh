#!/bin/sh
# #2664: a call to a `@c_callback("sym")` function by its Aether name calls
# the symbol the annotation binds, from a module that imports it too.
#
# The definition is the C function `sym`, but a call was emitted with the
# merged Aether name (`cbmod_triple`), which no C function carries: gcc
# failed with "implicit declaration of function 'cbmod_triple'".
#
# Fixture: lib/cbmod exports `triple` and `label` (bound to their own
# symbols), `nine_times`, which calls `triple` inside the module, and `pick`,
# a clause set whose second clause carries the annotation.
#
#   1. prog.ae calls each qualified (`cbmod.triple(4)`), and reads the clause
#      set back through its registered symbol;
#   2. prog_selective.ae calls selectively imported ones by their bare names.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] c_callback_call_by_name: $AE not built"
    exit 0
fi

fail=0
cd "$SCRIPT_DIR"

# runs <file> <expected last line> <label>
runs() {
    out="$(AETHER_HOME="" "$AE" run "$1" 2>&1 | tr -d '\r')"
    if [ "$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tail -1)" != "$2" ]; then
        echo "  [FAIL] $3: $1 did not print '$2':"
        printf '%s\n' "$out" | sed 's/^/        /' | head -15
        fail=1
        return 1
    fi
    echo "  [PASS] $3"
    return 0
}

runs prog.ae "12 18 M5 100 5 100" "qualified calls to a module's @c_callback functions"
runs prog_selective.ae "15 M6" "selectively imported @c_callback functions called by their bare names"

if [ "$fail" -eq 0 ]; then
    echo "PASS: c_callback_call_by_name (#2664)"
    exit 0
fi
echo "FAIL: c_callback_call_by_name (#2664)"
exit 1
