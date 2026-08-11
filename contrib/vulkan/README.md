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
the pipeline is built from whatever SPIR-V you hand it, and the vertex format is
an interleaved `vec2` position plus `vec3` colour.

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
test; the Linux CI leg runs the same test under valgrind as a leak gate.

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

`shaders/triangle.vert` and `.frag` are the GLSL sources; the `.spv` files beside
them are committed so that building needs no shader compiler. After editing the
GLSL, run `shaders/build_shaders.sh` and commit the result.

`tests/integration/vulkan_shaders` checks that the committed binaries are
well-formed SPIR-V and that the GLSL still compiles. It deliberately does not
byte-compare against a fresh glslang run, because the generator id and word
layout differ between glslang releases; that check would fail on version drift
rather than on a defect. What proves the pair is correct is the render test.

## Testing

`test_vulkan.ae` runs from `make contrib-check`. With no driver it prints SKIP
and passes, which is the same path a user's program takes. With a driver it
renders and checks pixels, covering the failure modes as well as the happy one:
zero and negative sizes, a size past `maxImageDimension2D`, empty SPIR-V, a
length that is not a multiple of 4, bytes that are not SPIR-V at all, vertex
indices out of range, writes before a reserve, pixel coordinates outside the
image, a null destination for readback, destroying null handles, and eight
create/draw/destroy cycles.

The Linux CI leg installs lavapipe so the GPU path runs on a runner with no GPU,
and then asserts the test did **not** skip. A skip there would be silent loss of
coverage.

## Not in phase 1

Surfaces, swapchains and presenting to a window; depth and stencil; textures and
descriptor sets; MSAA; compute. The seam with aether-ui, where a native window
handle is passed in, is a phase 2 decision and deliberately not pre-empted here.
