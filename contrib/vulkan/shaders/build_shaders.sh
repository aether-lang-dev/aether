#!/bin/sh
# Regenerates the checked-in SPIR-V from the GLSL beside it.
#
# The .spv files are committed so that building contrib/vulkan needs no shader
# compiler. Run this after editing a .vert, .frag or .comp, and commit the result.
set -e
cd "$(dirname "$0")"
command -v glslangValidator >/dev/null 2>&1 || {
    echo "glslangValidator not found (brew install glslang / apt install glslang-tools)" >&2
    exit 1
}
for src in triangle.vert triangle.frag transform.vert transform.frag \
           textured.vert textured.frag \
           depth.vert depth.frag \
           transform.comp vertices.comp pulled.vert \
           depth_read.frag volume.frag shadow.frag \
           instanced.vert plain.vert uniform_color.frag indirect_args.comp \
           cube.frag array.frag storage_write.comp storage_write3d.comp mrt.frag points.vert; do
    glslangValidator -V --target-env vulkan1.0 "$src" -o "$src.spv"
    echo "  $src -> $src.spv"
done
# Ray queries need SPIR-V 1.4, which the Vulkan 1.2 environment brings.
for src in ray_query.comp; do
    glslangValidator -V --target-env vulkan1.2 "$src" -o "$src.spv"
    echo "  $src -> $src.spv"
done
