#!/bin/sh
# Issue #2275: a top-level `when` in an IMPORTED module lost its surviving
# arm. The merge copies a module's declarations into the program, and a
# top-level `when` is not one of them, so the arm's extern never arrived and
# the module's own function using it failed with "Undefined function". The
# same `when` in the entry file worked.
#
# Fixture (lib/whenmod.ae): a platform `when` whose arms each declare an
# extern, a function and a constant, and a `when` that is dead on every
# platform and names a symbol that does not exist (it must be pruned, or the
# link fails). main.ae calls through the module.
#
# Acceptance: builds and runs, printing the selected arm, its constant, the
# pruned `when`'s surviving value and a positive pid.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) want="windows 1 42 true" ;;
    *) want="posix 2 42 true" ;;
esac

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] when_in_imported_module: program errored (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tail -1)
if [ "$got" != "$want" ]; then
    echo "  [FAIL] when_in_imported_module: expected '$want', got '$got'"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] when_in_imported_module: a module's top-level when keeps its surviving arm"
exit 0
