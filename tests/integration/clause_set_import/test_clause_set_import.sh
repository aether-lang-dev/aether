#!/bin/sh
# #2643: a function written as several clauses in a module keeps every clause
# when another file imports it.
#
# The module merge skipped a definition whose prefixed name the program
# already held, and the clause it had just merged was one: only the first
# clause of an imported set was merged, and the set was emitted as a single
# function whose literal pattern fell through to the default
# (`clauses.label(4)` returned "" instead of "n4"), or, for a set returning
# nothing, as C that gcc rejected. The prune after type checking walked only
# the first definition of a name, so a module helper only a later clause
# calls was dropped from the C.
#
# Fixture: lib/clauses exports a string set (`label`), a set returning
# nothing (`store_at`), a guarded set whose last clause calls the module's
# own `one()` (`sign`) and an unannotated set whose first clause returns
# nothing (`twice`); lib/hub re-exports `label`.
#
#   1. prog.ae imports clauses directly and checks every clause, with a
#      leak window over the string set;
#   2. prog_hub.ae reaches `label` through the re-exporting facade.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] clause_set_import: $AE not built"
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

runs prog.ae "imported clause sets ok" "an imported clause set keeps every clause"
runs prog_hub.ae "zero n7" "a clause set through a re-exporting facade keeps every clause"

if [ "$fail" -eq 0 ]; then
    echo "PASS: clause_set_import (#2643)"
    exit 0
fi
echo "FAIL: clause_set_import (#2643)"
exit 1
