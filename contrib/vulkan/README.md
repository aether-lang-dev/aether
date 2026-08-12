# contrib.vulkan: offscreen GPU rendering

Phase 1 of [#1495](https://github.com/aether-lang-dev/aether/issues/1495): a GPU
tier for Aether. This is the offscreen half, which is the part that runs on every
platform and in CI. Surface creation, swapchains and presenting to a window are
phase 2 and are not here.

It lives in `contrib/` rather than `std/` for the reason
[docs/stdlib-vs-contrib.md](../../docs/stdlib-vs-contrib.md) gives: Vulkan needs
an SDK, a loader and a driver, and on macOS it needs MoltenVK, which does not
ship with the OS. That fails the "minimal and well-scoped dependencies" test.

## What it does

```aether
import contrib.vulkan

main() {
    if vulkan.available() != 1 {
        println("no GPU: ${vulkan.last_error()}")
        return
    }

    dev = vulkan.device_create()
    defer vulkan.device_destroy(dev)

    target = vulkan.target_create(dev, 256, 256)
    defer vulkan.target_destroy(target)

    pipe = vulkan.pipeline_create(dev, target, vert_spv, vert_len, frag_spv, frag_len)
    defer vulkan.pipeline_destroy(pipe)

    vulkan.verts_reserve(target, 3)
    vulkan.verts_set(target, 0,  0.0, -0.7,  1.0, 0.0, 0.0)
    vulkan.verts_set(target, 1, -0.7,  0.7,  0.0, 1.0, 0.0)
    vulkan.verts_set(target, 2,  0.7,  0.7,  0.0, 0.0, 1.0)

    vulkan.draw(target, pipe, 0.0, 0.0, 0.0, 1.0)
    vulkan.save_ppm(target, "triangle.ppm")
}
```

`example_triangle.ae` is that program in full. It is not limited to a triangle:
the pipeline is built from whatever SPIR-V you hand it, and `pipeline_create`
uses a built-in vertex format of an interleaved `vec2` position plus `vec3`
colour.

For anything past that, `pipeline_create_ex` takes a vertex layout you
describe, a push-constant block, and the shader resources the shaders read:

```aether
// position(2) + normal(3) + uv(2), interleaved, stride 28
lay = vulkan.layout_create()
defer vulkan.layout_destroy(lay)
vulkan.layout_binding(lay, 0, 28, vulkan.PER_VERTEX)
vulkan.layout_attr(lay, 0, 0, vulkan.FORMAT_R32G32_SFLOAT, 0)
vulkan.layout_attr(lay, 1, 0, vulkan.FORMAT_R32G32B32_SFLOAT, 8)
vulkan.layout_attr(lay, 2, 0, vulkan.FORMAT_R32G32_SFLOAT, 20)

binds = vulkan.bindings_create()
defer vulkan.bindings_destroy(binds)
vulkan.bindings_texture(binds, 0)     // layout(binding = 0) uniform sampler2D
vulkan.bindings_uniform(binds, 1)     // layout(binding = 1) uniform Block

pipe = vulkan.pipeline_create_ex(dev, target, vs, vlen, fs, flen, lay, 64, binds)

tex = vulkan.texture_create(dev, w, h)
vulkan.texture_upload(tex, bytes.data(rgba), w * h * 4)
vulkan.set_texture(pipe, 0, tex)

vulkan.uniform_floats(pipe, 1, 4)     // a vec4 the shader reads
vulkan.uniform_float(pipe, 1, 0, 1.0)

vulkan.push_floats(target, 16)        // a mat4 pushed per draw
vulkan.push_float(target, 0, 1.0)

vulkan.verts_reserve_n(target, 4, 7)  // 4 vertices of 7 floats
vulkan.verts_set_float(target, 0, -1.0)
vulkan.indices_reserve(target, 6)     // two triangles from four vertices
vulkan.indices_set(target, 0, 0)
```

A texture must be uploaded before it is bound: an image that was never given
pixels has no defined contents to sample, so binding one is refused rather than
drawn. Push constants are capped at 128 bytes, the minimum every Vulkan device
guarantees. A pipeline owns one descriptor set, which is enough for a
transform, a material and its textures; per-draw sets are not yet a thing here.

Coordinates are Vulkan NDC. x and y run -1 to 1, **y points down**, and a
front-facing triangle is counter-clockwise.

## Nothing links against libvulkan

The loader is opened with `dlopen` at runtime and every entry point is fetched
through `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr`. Two consequences worth
being explicit about:

- A program using this module **builds and starts on a machine with no driver**.
  `vulkan.available()` returns 0 and the program degrades instead of failing to
  launch. Only the Vulkan headers are needed to build, and they are header-only.
- Device-level entry points come from `vkGetDeviceProcAddr`, which returns the
  driver's own function rather than the loader's dispatch trampoline.

That second point is measurable rather than folklore. The two pointers differ,
and the driver entry is cheaper per call:

```
vkResetFences via loader    12.2 ns/call
vkResetFences via driver    11.6 ns/call   (5.2% less)
```

## Measured cost

Apple M1 Pro, MoltenVK 1.4.2 over Metal, 512x512 R8G8B8A8, release build.

| operation | time | note |
|---|---:|---|
| `device_create` | 10.9 ms | once per process |
| `target_create` | 0.78 ms | once per size |
| `pipeline_create` | 13.1 ms | once per shader pair |
| `draw` + GPU sync | 0.350 ms | 2855 fps |
| `draw` forcing a re-record | 0.375 ms | 7% more |
| `read_rgba` (1 MiB) | 0.025 ms | 38.7 GiB/s |

Three things the design does to earn those numbers:

- **The command buffer is recorded once and resubmitted.** Re-recording is only
  7% here because a single-triangle frame is dominated by submit and fence wait;
  the gap widens with draw count. It re-records only when the pipeline, vertex
  count or clear colour actually changes.
- **The readback buffer is mapped for the target's lifetime**, so a frame costs
  no `vkMapMemory` round trip. The 38.7 GiB/s above is host memcpy bandwidth,
  which is what a readback should cost once the mapping is free.
- **Vertices are written straight into mapped GPU-visible memory.**
  `verts_set` stores into the buffer the GPU reads, so geometry crosses from
  Aether without an intermediate host array or a staging copy.

`leaks -atExit` reports 0 leaks for the example, the test and the C-level smoke
test, against a real driver.

The Linux CI leg runs the test for correctness but does **not** leak-gate it.
That is a measurement decision: the CI driver is lavapipe, whose LLVM JIT
valgrind cannot follow. One render reports around 13,000 errors from about
1,000 contexts, all inside libvulkan and the driver's worker threads, and the
"definitely lost" total moves between runs because the driver is `dlclose`d
before exit and valgrind then loses the pointers into it. Gating on that would
measure Mesa rather than this module. The eight create/draw/destroy cycles in
the test are what would surface accumulation on that leg.

## Building against it

`ae build --extra` cannot pass `-I`, so a project needs an `aether.toml`:

```toml
[[bin]]
name = "app"
path = "src/main.ae"
extra_sources = ["contrib/vulkan/aether_vulkan.c"]

[build]
cflags = "-I/opt/homebrew/include"   # or: $(pkg-config --cflags vulkan)
```

No `link_flags` entry is needed, which is the point of the runtime loading.

Headers from **1.3.204** onward work: that is what Ubuntu 22.04 ships and what
the Linux CI leg builds against, so using a symbol newer than that fails there.

| platform | headers | driver |
|---|---|---|
| Linux | `apt install libvulkan-dev` | vendor ICD, or `mesa-vulkan-drivers` for lavapipe on the CPU |
| macOS | `brew install vulkan-headers` | `brew install vulkan-loader molten-vk` |
| Windows | Vulkan SDK | vendor ICD |

## Shaders

`shaders/*.vert` and `*.frag` are the GLSL sources; the `.spv` files beside them
are committed so that building needs no shader compiler. After editing the GLSL,
run `shaders/build_shaders.sh` and commit the result. `triangle` is the built-in
layout, `transform` adds a push-constant mat4, and `textured` reads a
caller-described layout plus a sampler and a uniform.

`tests/integration/vulkan_shaders` checks that the committed binaries are
well-formed SPIR-V and that the GLSL still compiles. It deliberately does not
byte-compare against a fresh glslang run, because the generator id and word
layout differ between glslang releases; that check would fail on version drift
rather than on a defect. What proves the pair is correct is the render test.

## Testing

`test_vulkan.ae` and `test_vulkan_resources.ae` run from `make contrib-check`. With no driver it prints SKIP
and passes, which is the same path a user's program takes. With a driver it
renders and checks pixels, covering the failure modes as well as the happy one:
zero and negative sizes, a size past `maxImageDimension2D`, empty SPIR-V, a
length that is not a multiple of 4, bytes that are not SPIR-V at all, vertex
indices out of range, writes before a reserve, pixel coordinates outside the
image, a null destination for readback, destroying null handles, and eight
create/draw/destroy cycles.

`test_vulkan_resources.ae` covers the phase-2 surface the same way, by reading
pixels back rather than trusting a status code: a mat4 pushed per draw actually
mirrors the triangle, a 2x2 texture lands one texel per quadrant of an indexed
quad drawn from a caller-described position/normal/UV layout, and a tint uniform
scales what the sampler returned. It also checks the refusals: a duplicate
binding, an index past the vertex count, a short pixel upload, an over-large
push block, and binding a texture that has no pixels yet.

The Linux CI leg installs lavapipe so the GPU path runs on a runner with no GPU,
and then asserts the test did **not** skip. A skip there would be silent loss of
coverage.

## Not here yet

Every limitation below has an issue. Nothing here is a TODO in a comment or a
plan in someone's head.

| Missing | Issue | Priority |
|---|---|---|
| Surfaces, swapchains, presenting to a window, and the aether-ui handle seam | #1505 | P1 |
| Generating declarations from `vk.xml` rather than by hand | #1506 | P1 |
| A CI leak gate (lavapipe's JIT defeats valgrind; LSan module suppressions are the route) | #1507 | P1 |
| Per-draw descriptor sets, 16-bit indices, texture mipmaps | #1540 | P3 |
| A thread-safety contract (none is claimed today) | #1510 | P2 |
| Building and running contrib on Windows (the `_WIN32` branch is compiled, never run) | #1511 | P2 |
| Depth, stencil and MSAA | #1512 | P3 |
| Frames in flight, and a configurable GPU timeout (currently a fixed 5s) | #1513 | P3 |
| More colour formats, and PNG output rather than PPM | #1514 | P3 |
| Compute pipelines | #1515 | P3 |
