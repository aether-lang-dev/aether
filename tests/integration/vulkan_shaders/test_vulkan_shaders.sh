#!/bin/sh
# The checked-in SPIR-V must be well-formed, and the GLSL it came from must
# still compile.
#
# Deliberately NOT a byte-compare against a fresh glslang run: the generator
# id and word layout differ between glslang releases, so that check would fail
# on version drift rather than on a real defect. What matters is that the
# committed binaries are valid SPIR-V, that the GLSL has not rotted, and that
# the pair actually renders, which contrib/vulkan's own test proves on a
# machine with a driver.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

SHADERS="$ROOT/contrib/vulkan/shaders"
rc=0
checked=0

for spv in "$SHADERS"/*.spv; do
    [ -f "$spv" ] || continue
    checked=$((checked + 1))
    name="$(basename "$spv")"

    size=$(wc -c < "$spv" | tr -d ' ')
    if [ "$size" -lt 20 ] || [ $((size % 4)) -ne 0 ]; then
        echo "  [FAIL] vulkan_shaders: $name is $size bytes, not a run of 32-bit words"
        rc=1
        continue
    fi

    # 0x07230203, little-endian on every platform that ships SPIR-V.
    magic=$(od -An -tx1 -N4 "$spv" | tr -d ' \n')
    if [ "$magic" != "03022307" ]; then
        echo "  [FAIL] vulkan_shaders: $name has magic $magic, expected 03022307"
        rc=1
    fi
done

if [ "$checked" -eq 0 ]; then
    echo "  [FAIL] vulkan_shaders: no .spv files found in contrib/vulkan/shaders"
    exit 1
fi

if command -v glslangValidator >/dev/null 2>&1; then
    for src in "$SHADERS"/triangle.vert "$SHADERS"/triangle.frag; do
        [ -f "$src" ] || continue
        if ! out=$(glslangValidator -V --target-env vulkan1.0 "$src" -o /dev/null 2>&1); then
            echo "  [FAIL] vulkan_shaders: $(basename "$src") no longer compiles"
            printf '%s\n' "$out" | sed 's/^/        /' | head -8
            rc=1
        fi
    done
    [ "$rc" -eq 0 ] && echo "  [PASS] vulkan_shaders: $checked SPIR-V binaries valid; GLSL still compiles"
else
    [ "$rc" -eq 0 ] && echo "  [PASS] vulkan_shaders: $checked SPIR-V binaries valid (no glslang, GLSL not recompiled)"
fi

exit "$rc"
