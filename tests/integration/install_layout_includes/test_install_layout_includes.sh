#!/bin/sh
# Regression (#1420): runtime sources must not reach headers by a path that
# only exists in the source tree.
#
# runtime/libaether_caps.c had `#include "../include/libaether.h"`. In the
# source tree that resolves; in an install the runtime sits at
# share/aether/runtime/ while headers are under include/aether/, so it pointed
# at a directory that does not exist and EVERY cross-compile died there. Native
# builds never noticed, because they pass the include set from `ae cflags` and
# never rely on the relative path, which is why this shipped.
#
# Two static assertions, both cheap and portable (no zig, no install needed):
#   1. no runtime/ or std/ source escapes its own tree with `../include/`;
#   2. the public header the runtime includes by name is actually shipped by
#      both installers, since it lives outside the runtime/ and std/ trees
#      their header walks cover.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

fail=0

# (1) No source-tree-relative escape into include/.
hits=$(grep -rn '#include[[:space:]]*"\.\./include/' runtime std 2>/dev/null)
if [ -n "$hits" ]; then
    echo "  [FAIL] install_layout_includes: source-tree-relative include of the public header"
    echo "$hits" | sed 's/^/        /' | head -10
    echo "        Include it by name and let the -I set resolve it; the installed"
    echo "        layout has no share/aether/include/ directory."
    fail=1
fi

# (2) The public header is shipped by both install paths.
if [ -f include/libaether.h ]; then
    if ! grep -q 'include/\*\.h' install.sh; then
        echo "  [FAIL] install_layout_includes: install.sh does not ship include/*.h"
        fail=1
    fi
    if ! grep -q 'include/\*\.h' Makefile; then
        echo "  [FAIL] install_layout_includes: the Makefile install does not ship include/*.h"
        fail=1
    fi
else
    echo "  [FAIL] install_layout_includes: include/libaether.h is missing"
    fail=1
fi

[ "$fail" -eq 0 ] || exit 1
echo "  [PASS] install_layout_includes: public header shipped, no source-tree-relative includes"
