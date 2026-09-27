#!/bin/sh
# Issue #2190: a library `noise` imports std.math and calls `math.floor`.
# When the program also imports its own `pkg.math`, the two modules end in
# the same segment. Under the 0.712 exports enforcement `math.floor` inside
# noise resolved against pkg.math ("'floor' is not exported from module
# 'math'"); after #2209 gave both modules their full path it became
# "Undefined function 'std_math.floor'", because std.math's functions are
# runtime externs compiled as `math_*` and cannot follow a renamed
# namespace. `import std.math as stdmath` in the library failed the same way.
#
# A shipped module (std.*, contrib.*) now keeps its last segment whatever
# collides with it; only the local module (`pkg.math` -> `pkg_math`) moves.
#
# Fixture (lib/): pkg/math (the colliding local module), noise (imports
# std.math), noise_alias (imports std.math as stdmath). The two .ae drivers
# import the program's `math` first and the std-importing library first, so
# both load orders are exercised, plus a program-level alias of std.math.
#
# Acceptance: both drivers compile, link, run, and print their final
# "All ... pass" line with exit 0. Run from this directory so `./lib`
# is on the module search path.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

fail=0
for t in test_std_leaf_collision test_std_leaf_collision_order; do
    out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run "$t.ae" 2>&1 )
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  [FAIL] std_leaf_collision: $t errored (rc=$rc)"
        echo "$out" | head -20 | sed 's/^/          /'
        fail=1
        continue
    fi
    if ! printf '%s\n' "$out" | grep -q 'All .* pass'; then
        echo "  [FAIL] std_leaf_collision: $t did not report all cases passing"
        echo "$out" | head -20 | sed 's/^/          /'
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "  [PASS] std_leaf_collision: std.math keeps its namespace beside a local pkg.math, both import orders, aliases in libraries and programs"
exit 0
