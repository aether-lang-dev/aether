#!/bin/sh
# A module the program reaches only through selective imports is merged as
# a directly imported one is. The program imports only lib/mid.ae, which
# imports cmath, std.math, roots, probe and pinger selectively, so all of
# them were merged by the transitive pass. That pass kept its own copy of
# the merge, which cloned functions, constants and types but not:
#   - `@extern("sym") name(...)`s: roots' `cmath.root(x)` and `math.sqrt(x)`
#     were "Undefined function" (E0301);
#   - fault sets: probe's own `return "", Missing` was "Undefined variable
#     'probe_Missing'" (E0300);
#   - messages and actors: pinger's own `spawn(Pinger())` was "Undefined
#     function 'spawn_Pinger'" (E0301).
# A bare import anywhere on the path hid it, by merging the module directly.
#
# Acceptance, main.ae prints exactly:
#   0.909297 0.909297 probe.Missing probe.Denied found ping 7

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
NAME=transitive_selective_import_merge
WANT="0.909297 0.909297 probe.Missing probe.Denied found ping 7"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] $NAME: program errored (rc=$rc)"
    echo "$out" | head -30 | sed 's/^/          /'
    exit 1
fi

got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
if [ "$got" != "$WANT" ]; then
    echo "  [FAIL] $NAME: expected '$WANT', got '$got'"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] $NAME"
exit 0
