#!/bin/sh
# The release must ship every header at the top of include/, not just the
# runtime/ and std/ subtrees.
#
# include/aether/ is itself an -I directory for an installed tree, and
# share/aether/runtime/libaether_caps.c -- which the release also ships --
# does `#include "libaether.h"`. A release carrying that .c without the
# header cross-compiles hello.ae fine, because a trivial program pulls no
# runtime .c, and fails on anything that links the runtime:
#
#   libaether_caps.c:18:10: fatal error: 'libaether.h' file not found
#
# That shipped in 0.597.0. A local install was unaffected, which is what made
# it easy to miss: the Makefile's staging copies these (since #1420) and the
# release workflow reimplements that staging without the line.
#
# This asserts the CONTRACT rather than one filename: every .h directly in
# include/ has to be staged, so a new public header cannot go missing the
# same way.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
WF="$ROOT/.github/workflows/release.yml"

[ -f "$WF" ] || { echo "  [SKIP] release.yml not found"; exit 0; }
[ -d "$ROOT/include" ] || { echo "  [SKIP] no include/ in the tree"; exit 0; }

# Every packaging block that stages the runtime source must also stage the
# top-level headers. Counting both means a NEW packaging block (a new target)
# cannot quietly omit it.
n_pkg=$(grep -c 'cp -r runtime release/share/aether/' "$WF" || true)
n_hdr=$(grep -c 'cp include/\*\.h release/include/aether/' "$WF" || true)

[ "$n_pkg" -gt 0 ] || { echo "  [FAIL] no packaging block found in release.yml"; exit 1; }
if [ "$n_hdr" != "$n_pkg" ]; then
    echo "  [FAIL] $n_pkg packaging block(s) ship the runtime source but only"
    echo "         $n_hdr stage include/*.h into include/aether/."
    echo "         A release that ships runtime/libaether_caps.c without"
    echo "         libaether.h cannot cross-compile a runtime-linking program."
    exit 1
fi

# And the header the runtime actually needs must exist to be staged.
[ -f "$ROOT/include/libaether.h" ] || {
    echo "  [FAIL] include/libaether.h is missing from the tree"
    exit 1
}

# The include is by bare name, so it must resolve from an -I of include/aether
# rather than a relative path. If that ever changes to a relative form, the
# staging above stops being the right fix and this should be revisited.
CAPS="$ROOT/runtime/libaether_caps.c"
if [ -f "$CAPS" ]; then
    grep -q '#include "libaether\.h"' "$CAPS" || {
        echo "  [FAIL] runtime/libaether_caps.c no longer includes libaether.h by"
        echo "         bare name; the release staging assumption needs rechecking."
        exit 1
    }
fi

echo "  [PASS] release_ships_public_headers: $n_pkg packaging block(s) stage include/*.h"
