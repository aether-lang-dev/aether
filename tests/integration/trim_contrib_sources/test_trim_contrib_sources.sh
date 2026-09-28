#!/bin/sh
# .github/scripts/trim_contrib_sources.sh, the step install.sh and the
# Makefile's install targets run over the installed contrib tree (#2208):
# every .c and .m goes, except a file a module.ae in the tree names with
# @source (contrib.vulkan.vk names one a directory up), and, when asked, the
# contrib/host/<lang>/aether_host_<lang>.c bridges. Run on a tree built
# here, so it needs no install and no toolchain.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
TRIM="$ROOT/.github/scripts/trim_contrib_sources.sh"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail() { echo "  [FAIL] trim_contrib_sources: $1"; exit 1; }

# A tree with the shapes the real one has.
make_tree() {
    rm -rf "$1"
    mkdir -p "$1/vulkan/vk" "$1/metal" "$1/sqlite" "$1/host/python" "$1/host/lua"
    printf '@c_include("vulkan/vulkan.h")\n@source("aether_vulkan.c")\n' > "$1/vulkan/module.ae"
    printf '@c_include("vulkan/vulkan.h")\n@source("../aether_vulkan.c")\n' > "$1/vulkan/vk/module.ae"
    : > "$1/vulkan/aether_vulkan.c"
    : > "$1/vulkan/aether_vulkan.h"
    : > "$1/vulkan/scratch.c"
    printf '  @source("aether_metal.c")\n' > "$1/metal/module.ae"
    : > "$1/metal/aether_metal.c"
    : > "$1/metal/aether_metal_extra.m"
    printf '// links libaether_sqlite.a\n' > "$1/sqlite/module.ae"
    : > "$1/sqlite/aether_sqlite.c"
    : > "$1/host/python/module.ae"
    : > "$1/host/python/aether_host_python.c"
    : > "$1/host/python/aether_host_python.h"
    : > "$1/host/lua/aether_host_lua.c"
    # A module naming a file that is not there must not break the trim.
    mkdir -p "$1/ghost"
    printf '@source("missing.c")\n' > "$1/ghost/module.ae"
}

present() { [ -f "$1" ] || fail "$2: $1 should have been kept"; }
gone()    { [ ! -f "$1" ] || fail "$2: $1 should have been removed"; }

# --- plain install: host bridges kept ---------------------------------------
make_tree "$TMP/contrib"
sh "$TRIM" "$TMP/contrib" --keep-host-bridges || fail "exited non-zero with --keep-host-bridges"
present "$TMP/contrib/vulkan/aether_vulkan.c"        "keep-host: @source in the same directory"
present "$TMP/contrib/metal/aether_metal.c"          "keep-host: @source with leading spaces"
present "$TMP/contrib/host/python/aether_host_python.c" "keep-host: host bridge"
present "$TMP/contrib/host/lua/aether_host_lua.c"    "keep-host: host bridge without a module.ae"
present "$TMP/contrib/vulkan/aether_vulkan.h"        "keep-host: headers untouched"
present "$TMP/contrib/host/python/aether_host_python.h" "keep-host: bridge header untouched"
gone    "$TMP/contrib/vulkan/scratch.c"              "keep-host: a .c no module names"
gone    "$TMP/contrib/metal/aether_metal_extra.m"    "keep-host: a .m no module names"
gone    "$TMP/contrib/sqlite/aether_sqlite.c"        "keep-host: an archived module's source"

# --- install-contrib: archives carry the bridges, so they go too ------------
make_tree "$TMP/contrib"
sh "$TRIM" "$TMP/contrib" || fail "exited non-zero without the flag"
present "$TMP/contrib/vulkan/aether_vulkan.c"        "no-flag: @source kept"
present "$TMP/contrib/metal/aether_metal.c"          "no-flag: @source kept"
gone    "$TMP/contrib/host/python/aether_host_python.c" "no-flag: host bridge"
gone    "$TMP/contrib/host/lua/aether_host_lua.c"    "no-flag: host bridge"
gone    "$TMP/contrib/sqlite/aether_sqlite.c"        "no-flag: archived module's source"

# --- the one contrib.vulkan.vk needs: "../aether_vulkan.c" alone -----------
make_tree "$TMP/contrib"
rm "$TMP/contrib/vulkan/module.ae"
sh "$TRIM" "$TMP/contrib" || fail "exited non-zero on the ../ case"
present "$TMP/contrib/vulkan/aether_vulkan.c"        "a @source climbing out of vk/"

# --- behind a symlinked prefix, as ~/.aether may be ------------------------
# Only where `ln -s` makes a symlink. MSYS2 without winsymlinks copies the
# directory instead, so trimming through "link" would clean a copy and leave
# "real" untouched; that is not the case being tested.
make_tree "$TMP/real/contrib"
ln -s "$TMP/real" "$TMP/link" 2>/dev/null || true
if [ -L "$TMP/link" ]; then
    sh "$TRIM" "$TMP/link/contrib" || fail "exited non-zero through a symlink"
    present "$TMP/real/contrib/vulkan/aether_vulkan.c"   "symlinked prefix: @source kept"
    gone    "$TMP/real/contrib/vulkan/scratch.c"         "symlinked prefix: noise removed"
else
    echo "  (trim_contrib_sources: no symlinks here, symlinked-prefix case not run)"
fi

# --- a directory that is not there is not an error -------------------------
sh "$TRIM" "$TMP/nowhere" || fail "exited non-zero on a missing directory"

echo "  [PASS] trim_contrib_sources: @source'd C survives the install trim, the rest goes"
