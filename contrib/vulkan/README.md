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

`example_triangle.ae` is that program in full, `example_parallel_render.ae` is
four actors rendering their own tile on one shared device and writing a 2x2
contact sheet, and `example_sprites.ae` draws four differently textured sprites
in a single frame. All three are RUN by `make contrib-check`, not merely
compiled: an example nobody executes rots into decoration.

None of it is limited to a triangle: the pipeline is built from whatever SPIR-V
you hand it, and `pipeline_create` uses a built-in vertex format of an
interleaved `vec2` position plus `vec3` colour.

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
guarantees.

Coordinates are Vulkan NDC. x and y run -1 to 1, **y points down**, and a
front-facing triangle is counter-clockwise.

## Materials: several textures in one frame

The calls above bind to the pipeline, which owns one descriptor set. That is
enough for one object. A frame with two textures needs two sets, because the
second bind would otherwise overwrite what the first draw is still going to
read, and a `vkQueueWaitIdle` between draws is not a fix.

A material is one set of bound resources. Several are made from one pipeline,
and a frame holds a list of draws, each naming the material it uses:

```aether
mat_a = vulkan.material_create(pipe)
mat_b = vulkan.material_create(pipe)
defer vulkan.material_destroy(mat_a)
defer vulkan.material_destroy(mat_b)

vulkan.material_set_texture(mat_a, 0, tex_a)
vulkan.material_set_texture(mat_b, 0, tex_b)
vulkan.material_floats(mat_a, 1, 4)          // a vec4 per material
vulkan.material_float(mat_a, 1, 0, 1.0)

vulkan.batch_add(target, mat_a, 0, 6)        // indices 0..5 with mat_a
vulkan.batch_add(target, mat_b, 6, 6)        // indices 6..11 with mat_b
vulkan.draw_material(target, pipe, null, 0.0, 0.0, 0.0, 1.0)
```

`batch_add` slices the geometry already uploaded: by index when the target has
an index buffer, by vertex otherwise. An empty batch, which is the default and
what every earlier caller has, draws all of it once. `batch_reset` goes back to
that. The range is checked when the draw is added and again for the frame it is
drawn in, since geometry can be re-uploaded in between.

`draw_material` blocks like `draw`; `submit_material` pipelines like `submit`.
Passing a null material uses the pipeline's own set, so `set_texture`,
`uniform_floats` and `uniform_float` keep working unchanged.

Materials cost one descriptor set each, allocated from pools of 16 that the
pipeline grows on demand, plus one host-mapped uniform buffer per uniform
binding written. A set is bound only once something has been written into it: a
set straight out of the pool holds no descriptors, and a software rasteriser
walks a set as it is bound, so binding an empty one crashed Mesa 22.3 inside
the driver. A material a batch refers to has to outlive the draws that use
it: destroy one without resetting the batch and the next frame reads freed
memory, the same contract Vulkan gives for any resource bound to a set.

## 16-bit indices

`indices_reserve` gives 32-bit indices. `indices_reserve_ex(t, count, 16)`
halves the index buffer, which is the right width for any mesh under 65536
vertices:

```aether
vulkan.indices_reserve_ex(target, 24, 16)
vulkan.indices_set(target, 0, 0)
```

`indices_set` refuses a value the chosen width cannot hold, so a wrapped index
cannot silently draw the wrong triangle. Changing the width reallocates, and
the previous contents do not carry over: write the indices again after
reserving.

## Mipmaps and sampler options

`texture_create` gives a single-level image sampled with a linear filter and
clamped addressing. `texture_create_ex` chooses:

```aether
tex = vulkan.texture_create_ex(dev, 128, 128, 1, 1, 0)  // mipmapped, linear, clamped
vulkan.texture_upload(tex, bytes.data(rgba), 128 * 128 * 4)
vulkan.texture_mip_levels(tex)                          // 8
```

The chain is generated on upload with `vkCmdBlitImage`, level by level, so the
pixels come from the same call that already staged them. That needs the device
to advertise all three features the blit requires for `R8G8B8A8_UNORM`,
`BLIT_SRC`, `BLIT_DST` and `SAMPLED_IMAGE_FILTER_LINEAR`; where it does not,
creation fails with that reason rather than handing back a texture whose lower
levels are empty.

The difference is measurable rather than decorative. A 128x128 one-texel
checkerboard drawn into 16 pixels, which is eight times minification, comes out
of a non-mipmapped texture as pure black and white texels (mean brightness 255
on this hardware: the sample points all landed on the white squares) and out of
a mipmapped one at 128, the texture's actual average. `test_vulkan_materials.ae`
asserts both.

## Depth and multisampling

`target_create` gives one colour attachment at one sample. `target_create_ex`
adds either or both:

```aether
// depth on, 4x multisampling
t = vulkan.target_create_ex(dev, 512, 512, 1, 4)
```

**Depth** makes overlapping geometry resolve by distance instead of by
submission order, which is the painter's-algorithm ceiling a GPU tier exists to
escape. The format is chosen from what the device reports for optimal-tiling
depth attachments, preferring plain depth over combined depth+stencil so a
caller who never reads a stencil does not pay for one. A pipeline built for a
depth target tests and writes depth with a LESS comparison; the buffer clears
to the far plane.

Your vertex shader has to supply a z. The built-in layout is `vec2`, so a depth
pipeline wants a caller-described layout with a `vec3` position, which is what
`shaders/depth.vert` does.

**Multisampling** takes 1, 2, 4, 8 or 16, checked against
`framebufferColorSampleCounts & framebufferDepthSampleCounts` rather than
rounded down silently: asking for 4x on hardware that offers 2x is an error
with a message. Above one sample the colour attachment is multisampled and
resolves into the single-sample image, so `pixel()`, `copy_rgba` and
`save_ppm` are unchanged.

Both multisampled attachments are `TRANSIENT`: they exist only inside the
render pass, so a tiler never writes them to memory.

`target_has_depth(t)` and `target_samples(t)` report what a target actually
got.

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

## Threads

**One device may be used from several threads.** Vulkan requires the caller to
synchronise a `VkQueue` and a `VkCommandPool`; this module does that with one
lock per device, taken across queue submission and every command-pool access.
Aether is an actor language, so two actors sharing a device is the ordinary
shape, and leaving it undefined would be a trap rather than a simplification.

What that covers and what it does not:

| | |
|---|---|
| Two actors drawing to their own targets from one device | Safe |
| Creating and destroying targets or textures concurrently | Safe |
| Two threads drawing to the **same** target | Not safe: a target owns one command buffer and one fence, so serialise it or give each thread its own |
| Destroying a device while another thread is using it | Not safe, and no lock can help: the lock lives inside the object being freed |
| `last_error()` | Thread-local, so read it on the thread that made the failing call. On another thread it reports `""` |

`available()` is safe to call concurrently. Its probe is serialised, because it
fills a 256-byte device-name buffer that `device_name()` reads.

The cost is not measurable on the offscreen path: the same two-actor workload
takes the same wall time with the lock compiled out, since a draw already
blocks on a fence. `test_vulkan_actors.ae` exercises the contract, and its
header is explicit that passing does not prove the lock is load-bearing on any
particular driver.

## Frames in flight

`draw` records, submits and blocks on a fence. That is the right shape for a
deterministic offscreen render, and it is a hard ceiling: the CPU idles for the
whole GPU execution. `submit` hands the work to the queue and returns, so the
CPU records the next frame while the GPU runs this one.

```aether
vulkan.target_set_frames(t, 3)        // 1..8; 1 is the default and synchronous
vulkan.submit(t, pipe, r, g, b, 1.0)  // returns a slot, or a negative status
vulkan.wait_all(t)                    // drain, when you want the queue empty
```

Measured on an M1 Pro, 512x512, 300 frames, each with a different clear colour
so no submission is served from the recorded command buffer:

| Frames in flight | Per frame | |
|---|---:|---|
| 1 (synchronous) | 351 us | |
| 2 | 161 us | **2.2x** |
| 3 | 155 us | 2.3x |

Each slot owns its command buffer, its fence **and its own readback buffer**,
because a shared one would let frame N+1 overwrite pixels frame N had not been
read yet. That is the cost of the feature: `width * height * 4` per slot, which
is why it is opt-in rather than a default of 2 or 3.

`pixel`, `copy_rgba` and `save_ppm` wait for the newest submitted frame before
reading, so a caller who never calls `wait_all` still sees a whole frame rather
than one the GPU is mid-way through writing. The recorded-command cache is per
slot, so changing geometry or push constants invalidates all of them.

`target_set_timeout_ms(t, ms)` sets the fence wait; the default 5000 is a hang
detector rather than a frame budget.

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

`test_vulkan.ae`, `test_vulkan_resources.ae`, `test_vulkan_actors.ae`,
`test_vulkan_depth_msaa.ae`, `test_vulkan_frames.ae` and
`test_vulkan_materials.ae` run from `make contrib-check`, along with all three
examples. With no driver it prints SKIP
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

`test_vulkan_actors.ae` drives the threading contract: two actors, one device,
each drawing to its own target and churning a scratch target through the shared
command pool, each checking its own colour every frame, plus several hundred
concurrent `available()` / `device_name()` calls that must all agree.

Its header is explicit about the limit of that: compiled with the lock removed
it still passes on MoltenVK, and ThreadSanitizer does not flag the probe race
either, because the window is a few microseconds at process start. What the
lock buys is that the behaviour is DEFINED by the specification rather than
tolerated by one driver. A test can show the contract holding; it cannot show a
race is absent.

`test_vulkan_depth_msaa.ae` is written so that a feature doing nothing fails
it. The depth case draws the same two overlapping triangles in both submission
orders and requires the overlap to match, then runs the identical comparison on
a target with no depth attachment and requires it to DISAGREE. The MSAA case
counts pixels that are neither background nor a saturated primary: 0 at one
sample, 105 at four on this hardware.

`test_vulkan_materials.ae` is built so each of the three features fails it if
it does nothing. Two quads with different textures are drawn in one frame and
each half is checked for its own colour, which is exactly what one shared
descriptor set cannot produce. The 16-bit frame is compared to the 32-bit one
byte for byte over the whole readback rather than at a few sample points. The
mipmap case requires every pixel of the minified quad to be a single texel
without a chain and the averaged mean with one. It also checks the refusals: a
draw past the uploaded geometry, at both add and draw time, an empty or
negative draw range, an index width that is neither 16 nor 32, a material
without a pipeline, and a uniform write to a binding that has no buffer.

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
| Building and running contrib on Windows (the `_WIN32` branch is compiled, never run) | #1511 | P2 |
| More colour formats, and PNG output rather than PPM | #1514 | P3 |
| Compute pipelines | #1515 | P3 |
