#!/bin/sh
# Issue #2209: two modules whose paths end in the same segment (`gfx.vk`,
# imported by library `pb`, and the program's own `mine.vk`) used to share
# one namespace program-wide. Whichever was imported first answered for
# both, so the other's exports were reported missing; with the imports
# swapped the failure moved to the other module. `import X as Y` inside a
# library module was ignored, and a program-level alias on a local package
# merged nothing.
#
# A module's imports now resolve in that module, by full path or alias,
# rather than program-wide by last segment. Each registered module gets a
# namespace: its last segment, or its full path with dots as underscores
# (`mine_vk`, `gfx_vk`) when another loaded module ends the same way.
#
# Fixture (lib/): mine/vk and gfx/vk (the colliding leaf), pb (imports
# gfx.vk), pa (aliases a package). The two .ae drivers import the program's
# `vk` first and the library-loaded `vk` first, so both import orders are
# exercised, plus alias-in-library, program-level alias, and a local that
# shadows an alias.
#
# Acceptance: both drivers compile, link, run, and print their final
# "All ... pass" line with exit 0. Run from this directory so `./lib`
# is on the module search path.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

fail=0
for t in test_module_leaf_collision test_module_leaf_collision_order; do
    out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run "$t.ae" 2>&1 )
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  [FAIL] module_leaf_collision: $t errored (rc=$rc)"
        echo "$out" | head -20 | sed 's/^/          /'
        fail=1
        continue
    fi
    if ! printf '%s\n' "$out" | grep -q 'All .* pass'; then
        echo "  [FAIL] module_leaf_collision: $t did not report all cases passing"
        echo "$out" | head -20 | sed 's/^/          /'
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "  [PASS] module_leaf_collision: same-leaf modules resolve per-module, both import orders, aliases in libraries and programs"
exit 0
