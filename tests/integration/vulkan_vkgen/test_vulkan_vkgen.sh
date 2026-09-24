#!/bin/sh
# contrib/vulkan's generated files match what tools/vkgen.ae generates from
# the registry installed here (#1506), and what it generates compiles.
#
#   aether_vulkan_dispatch.h  must be identical, apart from the line naming the
#                             registry release: the entry points and the
#                             feature or extension each came from are the
#                             same in every release that has them.
#   vk/module.ae              must be identical when the installed registry is
#                             the release it was generated from; a later
#                             release adds enum values and aliases, so across
#                             releases it is compiled and run instead.
#
# Both regenerated files are then built together: the module against the
# installed <vulkan/vulkan.h>, and aether_vulkan.c (which the module compiles
# in) against the regenerated dispatch header.
#
# Skips where no vk.xml is installed. The Linux contrib job installs one and
# sets VKGEN_REQUIRE_RELEASE=1: the committed files are generated from the
# registry that job installs (the oldest this repository builds against), so
# there both are compared byte for byte, and a release that differs fails
# rather than turning the module comparison off.
set -eu

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
AE="$ROOT/build/ae"
if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] vulkan_vkgen: build/ae not built"
    exit 0
fi
AETHER_HOME=""
export AETHER_HOME AE

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail() {
    echo "  [FAIL] vulkan_vkgen: $1"
    exit 1
}

if ! sh "$ROOT/contrib/vulkan/tools/regenerate.sh" --out "$WORK/gen" >"$WORK/regen.log" 2>&1; then
    if grep -q "no vk.xml found" "$WORK/regen.log"; then
        echo "  [SKIP] vulkan_vkgen: no Vulkan registry (vk.xml) installed"
        exit 0
    fi
    sed 's/^/    /' "$WORK/regen.log" | tail -20
    fail "regenerate.sh failed"
fi

COMMITTED="$ROOT/contrib/vulkan"
release() { sed -n 's/.*Registry: \([0-9.]*[0-9]\)\..*/\1/p' "$1" | head -1; }
have="$(release "$WORK/gen/aether_vulkan_dispatch.h")"
want="$(release "$COMMITTED/aether_vulkan_dispatch.h")"
want_module="$(release "$COMMITTED/vk/module.ae")"
[ -n "$have" ] && [ -n "$want" ] && [ -n "$want_module" ] || fail "a generated file does not name its registry release"
[ "$want" = "$want_module" ] || fail "the committed dispatch header ($want) and module ($want_module) come from different registries"
if [ "${VKGEN_REQUIRE_RELEASE:-0}" = "1" ] && [ "$have" != "$want" ]; then
    fail "the committed files come from registry $want and this registry is $have: regenerate them from this one (contrib/vulkan/tools/regenerate.sh --registry <its vk.xml>)"
fi

grep -v "Registry: " "$COMMITTED/aether_vulkan_dispatch.h" > "$WORK/committed.h"
grep -v "Registry: " "$WORK/gen/aether_vulkan_dispatch.h" > "$WORK/generated.h"
if ! diff -u "$WORK/committed.h" "$WORK/generated.h" > "$WORK/dispatch.diff"; then
    sed 's/^/    /' "$WORK/dispatch.diff" | head -30
    fail "aether_vulkan_dispatch.h differs from what vkgen generates; run contrib/vulkan/tools/regenerate.sh"
fi

if [ "$have" = "$want" ]; then
    if ! diff -u "$COMMITTED/vk/module.ae" "$WORK/gen/vk/module.ae" > "$WORK/module.diff"; then
        sed 's/^/    /' "$WORK/module.diff" | head -30
        fail "vk/module.ae differs from what vkgen generates from registry $have; run contrib/vulkan/tools/regenerate.sh"
    fi
    module_check="identical"
else
    module_check="generated from $have (committed from $want)"
fi

# The regenerated module in a tree of its own, next to the C it compiles in.
cp "$COMMITTED/aether_vulkan.c" "$COMMITTED/aether_vulkan.h" "$WORK/gen/"
cat > "$WORK/gen/probe.ae" <<'AE'
import vk

extern calloc(n: int, size: int) -> ptr
extern free(p: ptr)

main() {
    // A command that returns a VkResult with no loader returns
    // ERROR_INITIALIZATION_FAILED; with one, the loader's answer.
    ver = calloc(1, 4)
    r = vk.vkEnumerateInstanceVersion(ver)
    if vk.loader_available() == 1 && r != vk.SUCCESS { println("vkEnumerateInstanceVersion: ${r}") }
    if vk.loader_available() == 0 && r != vk.ERROR_INITIALIZATION_FAILED { println("no loader, yet ${r}") }
    info = calloc(1, sizeof(VkApplicationInfo)) as *VkApplicationInfo
    info.sType = vk.STRUCTURE_TYPE_APPLICATION_INFO
    info.pApplicationName = "probe"
    println("${info.pApplicationName} ${info.sType}")
    free(info)
    free(ver)
}
AE
( cd "$WORK/gen" && "$AE" build probe.ae -o probe ) > "$WORK/build.log" 2>&1 || {
    sed 's/^/    /' "$WORK/build.log" | tail -25
    fail "the regenerated module and dispatch header do not build"
}
out="$("$WORK/gen/probe" 2>&1 | tr -d '\r')"
[ "$out" = "probe 0" ] || fail "the probe printed '$out', expected 'probe 0'"

echo "  [PASS] vulkan_vkgen: dispatch header identical, module $module_check, both build"
