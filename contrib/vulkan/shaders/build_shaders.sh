#!/bin/sh
# Regenerates the checked-in SPIR-V from the GLSL beside it.
#
# The .spv files are committed so that building contrib/vulkan needs no shader
# compiler. Run this after editing a .vert or .frag, and commit the result.
set -e
cd "$(dirname "$0")"
command -v glslangValidator >/dev/null 2>&1 || {
    echo "glslangValidator not found (brew install glslang / apt install glslang-tools)" >&2
    exit 1
}
for src in triangle.vert triangle.frag transform.vert transform.frag \
           textured.vert textured.frag; do
    glslangValidator -V --target-env vulkan1.0 "$src" -o "$src.spv"
    echo "  $src -> $src.spv"
done
