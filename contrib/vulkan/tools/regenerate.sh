#!/bin/sh
# Regenerates contrib/vulkan's files that are generated from the Vulkan
# registry (vk.xml) by tools/vkgen.ae:
#
#   aether_vulkan_dispatch.h   the entry points aether_vulkan.c loads, from
#                              tools/dispatch_commands.txt
#   vk/module.ae               contrib.vulkan.vk, the API as Aether declarations
#
#   contrib/vulkan/tools/regenerate.sh [--registry <vk.xml>] [--out <dir>]
#
# vk.xml ships with the Vulkan headers (registry/vk.xml in Khronos'
# Vulkan-Headers). Without --registry it is looked for under $VULKAN_SDK, the
# prefix pkg-config reports for vulkan, and the usual install prefixes.
# --out writes both files under <dir> (<dir>/aether_vulkan_dispatch.h,
# <dir>/vk/module.ae) instead of into the tree, to compare against what is
# committed. $AE names the `ae` to build vkgen with (default: build/ae).
set -eu

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
AE="${AE:-$ROOT/build/ae}"
OUT="$ROOT/contrib/vulkan"
REGISTRY=""

# The selections the repository commits. The dispatch header takes core 1.0
# because every command aether_vulkan.c loads is a 1.0 one; vkgen stops if a
# listed command is outside the selection.
DISPATCH_API=1.0
DISPATCH_EXTENSIONS=VK_KHR_surface,VK_KHR_swapchain,VK_KHR_win32_surface,VK_KHR_xlib_surface,VK_KHR_wayland_surface,VK_EXT_metal_surface
MODULE_API=1.3
MODULE_EXTENSIONS=VK_KHR_surface,VK_KHR_swapchain

while [ $# -gt 0 ]; do
    case "$1" in
        --registry) REGISTRY="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        -h|--help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "regenerate.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done

if [ -z "$REGISTRY" ]; then
    for prefix in "${VULKAN_SDK:-}" \
                  "$(pkg-config --variable=prefix vulkan 2>/dev/null || true)" \
                  /usr /usr/local /opt/homebrew /ucrt64 /mingw64; do
        if [ -n "$prefix" ] && [ -f "$prefix/share/vulkan/registry/vk.xml" ]; then
            REGISTRY="$prefix/share/vulkan/registry/vk.xml"
            break
        fi
    done
fi
if [ -z "$REGISTRY" ] || [ ! -f "$REGISTRY" ]; then
    echo "regenerate.sh: no vk.xml found; install the Vulkan headers or pass --registry" >&2
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
"$AE" build "$ROOT/contrib/vulkan/tools/vkgen.ae" -o "$WORK/vkgen" >"$WORK/build.log" 2>&1 || {
    cat "$WORK/build.log" >&2
    exit 1
}

mkdir -p "$OUT/vk"
"$WORK/vkgen" --registry "$REGISTRY" --api "$DISPATCH_API" --extensions "$DISPATCH_EXTENSIONS" \
    --commands "$ROOT/contrib/vulkan/tools/dispatch_commands.txt" \
    --dispatch "$OUT/aether_vulkan_dispatch.h"
"$WORK/vkgen" --registry "$REGISTRY" --api "$MODULE_API" --extensions "$MODULE_EXTENSIONS" \
    --module "$OUT/vk/module.ae"
