#!/bin/sh
# Two module_assign_namespaces (#2209) interactions with a same-last-segment
# collision, found rebuilding aether-ui's apps/frames_demo against a
# from-source `main`:
#
#   1. A scope whose own imports reduce to the SAME written prefix twice
#      (one selective, one whole-module, both renamed off the shared leaf)
#      had the first import's rewrite claim every qualified call in the
#      body, including the second import's own -- misrouting it to a module
#      that never defined it.
#   2. A parameter or local named identically to the module it forwards to
#      (a same-named forwarding wrapper, a real aether-ui pattern) blocked
#      the qualified-call rewrite entirely, for both a function call and a
#      constant read shaped like member access.
#
# Fixture (lib/): geo/thing.ae and thing.ae (the colliding leaf), consumer1
# (both imports in one scope -- case 1), consumer2 (the shadowing parameter
# pattern -- case 2).
#
# Acceptance: the driver compiles, links, runs, and prints its final
# "All ... pass" line with exit 0. Run from this directory so `./lib` is on
# the module search path.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run test_region_basename_collision_shadow.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] region_basename_collision_shadow: errored (rc=$rc)"
    echo "$out" | head -30 | sed 's/^/          /'
    exit 1
fi
if ! printf '%s\n' "$out" | grep -q 'All .* pass'; then
    echo "  [FAIL] region_basename_collision_shadow: did not report all cases passing"
    echo "$out" | head -30 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] region_basename_collision_shadow: ambiguous same-prefix imports and shadowing-parameter wrappers resolve correctly"
exit 0
