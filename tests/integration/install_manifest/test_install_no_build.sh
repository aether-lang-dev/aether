#!/bin/sh
# #2142: `AETHER_INSTALL_NO_BUILD=1 ./install.sh <prefix>` installs the
# binaries the tree already has instead of rebuilding them — and refuses
# when they are missing, so it can never install nothing.
#
# The three drivers that install into a temp prefix (this directory's two
# and install_contrib_resolves) each rebuilt the toolchain, relinking
# build/ae in the shared tree; on parallel workers they collided on the
# Windows file lock. They now pass the variable, and this test pins the
# guard that makes the variable safe: a tree with no build/ae and no
# build/aetherc exits 1 with a message naming the variable, and creates no
# prefix.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

# A tree that passes install.sh's own "run from the repository root" check
# (install.sh + Makefile beside each other) but has nothing built.
mkdir -p "$TMPDIR/tree"
cp "$ROOT/install.sh" "$ROOT/Makefile" "$TMPDIR/tree/"

AETHER_INSTALL_NO_BUILD=1 sh "$TMPDIR/tree/install.sh" "$TMPDIR/prefix" \
    < /dev/null > "$TMPDIR/install.log" 2>&1
rc=$?
if [ "$rc" = 0 ]; then
    echo "  [FAIL] install_no_build: install.sh exited 0 with no binaries to install"
    tail -5 "$TMPDIR/install.log" | sed 's/^/        /'
    exit 1
fi
if ! grep -q "AETHER_INSTALL_NO_BUILD=1 but build/ae and build/aetherc are not built" "$TMPDIR/install.log"; then
    echo "  [FAIL] install_no_build: the refusal does not name the variable and the missing binaries"
    tail -5 "$TMPDIR/install.log" | sed 's/^/        /'
    exit 1
fi
if [ -e "$TMPDIR/prefix" ]; then
    echo "  [FAIL] install_no_build: a prefix was created although nothing was installed"
    exit 1
fi
# The refusal must come BEFORE any make: nothing was built in the tree.
if [ -e "$TMPDIR/tree/build" ]; then
    echo "  [FAIL] install_no_build: install.sh built something despite AETHER_INSTALL_NO_BUILD=1"
    exit 1
fi

echo "  [PASS] install_no_build: AETHER_INSTALL_NO_BUILD=1 refuses a tree with no binaries and builds nothing"
exit 0
