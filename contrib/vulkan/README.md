# contrib.vulkan: GPU rendering, compute and presentation

The Vulkan member of Aether's GPU tier
([#1495](https://github.com/aether-lang-dev/aether/issues/1495)): offscreen
targets, pipelines, textures, materials, frames in flight, colour formats,
compute, and swapchains over a window someone else owns. For whatever that
does not cover, `contrib.vulkan.vk` is the Vulkan API itself, generated from
the registry. [`contrib.d3d12`](../d3d12/README.md) and
[`contrib.metal`](../metal/README.md) have the same shape over Direct3D 12 and
Metal; [docs/gpu.md](../../docs/gpu.md) covers what the three share and how to
pick one.

It lives in `contrib/` rather than `std/` for the reason
[docs/stdlib-vs-contrib.md](../../docs/stdlib-vs-contrib.md) gives: Vulkan needs
an SDK, a loader and a driver, and on macOS it needs MoltenVK, which does not
ship with the OS. That fails the "minimal and well-scoped dependencies" test.

## What it does

```aether,fragment
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

```aether,fragment
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

```aether,fragment
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

```aether,fragment
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

```aether,fragment
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

```aether,fragment
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

## Colour formats and image files

`target_create` renders to RGBA8 UNORM. `target_create_format` takes any of
four formats, each checked against the device before anything is made:

| Format | What it stores | Bytes a pixel |
|---|---|---:|
| `FORMAT_R8G8B8A8_UNORM` | 0..1 as 0..255 | 4 |
| `FORMAT_R8G8B8A8_SRGB` | linear shader output encoded for display: 0.5 is stored as 188 | 4 |
| `FORMAT_R16G16B16A16_SFLOAT` | half floats, not clamped: HDR light, accumulation | 8 |
| `FORMAT_R32G32B32A32_SFLOAT` | single floats, not clamped: values a shader computes | 16 |

```aether,fragment
t = vulkan.target_create_format(dev, 512, 512, vulkan.FORMAT_R16G16B16A16_SFLOAT, 1, 4)
vulkan.draw(t, pipe, 0.0, 0.0, 0.0, 1.0)
v = vulkan.pixel_value(t, 256, 256, 0)   // red as stored: 2.5 stays 2.5
err = vulkan.save_png(t, "frame.png")    // "" on success, the reason otherwise
```

`pixel_value` reads one channel at full precision. `pixel`, `copy_rgba8` and
`save_png` give 8-bit RGBA whatever the format, float channels clamped to 0..1.
`copy_rgba` gives the format's own bytes (`rgba_size()` of them). PNG output
goes through [`contrib.png`](../png/README.md), which encodes over `std.zlib`.
`save_ppm` remains for a file with no compressor in the path.

## Presenting to a window

```aether,fragment
sc = vulkan.swapchain_create(dev, vulkan.WINDOW_WIN32, null, hwnd, w, h)
defer vulkan.swapchain_destroy(sc)
vulkan.target_set_readback(target, 0)   // shown only: no copy back to host memory
// each frame:
vulkan.submit(target, pipe, 0.0, 0.0, 0.0, 1.0)
vulkan.present(sc, target)
```

The window is never this module's. The language does not own windowing
([#1505](https://github.com/aether-lang-dev/aether/issues/1505)): a swapchain is
made over the handle that whoever owns the window hands out. The kinds are
numbered as aether-ui's `native_view_kind()` numbers them, so its handle passes
straight through:

| Kind | `display` | `window` | Surface extension |
|---|---|---|---|
| `WINDOW_WIN32` (1) | null | `HWND` | `VK_KHR_win32_surface` |
| `WINDOW_NSVIEW` (2) | null | `NSView*` | `VK_EXT_metal_surface` |
| `WINDOW_X11` (3) | `Display*` | `Window` | `VK_KHR_xlib_surface` |
| `WINDOW_WAYLAND` (4) | `wl_display*` | `wl_surface*` | `VK_KHR_wayland_surface` |
| `WINDOW_METAL_LAYER` (5) | null | `CAMetalLayer*` | `VK_EXT_metal_surface` |

The instance enables each surface extension the loader offers and this build
has headers for. A kind whose extension is missing is refused with the
extension named, so one program presents wherever its loader can and says why
where it cannot. An `NSView` is given a `CAMetalLayer` at the window's backing
scale, on the main thread. That goes through the Objective-C runtime, so no
framework is linked.

- **Scaling and colour.** `present` blits the target's newest frame into the
  acquired image, scaling it when the two differ in size, and does not wait
  for the GPU. The swapchain is sRGB when the target is sRGB or float, so an
  sRGB target is shown as stored and a float target's linear light is encoded
  for display. Depth, MSAA, materials and frames in flight reach the screen
  unchanged.
- **Resizing.** Every present compares the surface's current size with the
  swapchain's and rebuilds on a change, because a driver need not report a
  resized or minimised window as out of date. `swapchain_resize` does it at
  once from a toolkit's resize hook. `target_resize` gives the target new
  images at the new size and keeps its render pass, so every pipeline made for
  it keeps working.
- **Minimised** (no area): presenting shows nothing and succeeds, and
  `swapchain_width()` is 0 until the window comes back.
- **Vsync** is on by default (FIFO, never tearing). `swapchain_set_vsync(sc, 0)`
  presents as soon as a frame is ready: MAILBOX where the surface offers it,
  IMMEDIATE otherwise.

## Compute

```aether,fragment
b = vulkan.bindings_create()
vulkan.bindings_storage(b, 0)              // layout(std430, binding = 0) buffer
c = vulkan.compute_create(dev, spv, spv_len, b, 4)
buf = vulkan.buffer_create(dev, 1024 * 4)
vulkan.compute_set_buffer(c, 0, buf)
vulkan.compute_push_float(c, 0, 2.0)
vulkan.dispatch(c, 16, 1, 1)               // waits; dispatch_async does not
v = vulkan.buffer_float(buf, 7)
```

Buffers are host-visible, coherent and mapped for their lifetime, and zeroed
when made, so results are read without a copy once the dispatch is done. A
buffer can be a storage or uniform binding of a compute pass and of a graphics
pipeline (`set_buffer`, `material_set_buffer`). A pipeline made with an empty
vertex layout takes no vertex input, so its vertex shader can pull what a
compute pass wrote by `gl_VertexIndex`, with no copy in between. A dispatch is
refused, with the reason named, when a declared binding was never set or a
group count exceeds the device's `maxComputeWorkGroupCount`. `dispatch_async`
returns at once, and `compute_wait` waits before the results are read or the
pass dispatched again.

## Nothing links against libvulkan

The loader is opened with `dlopen` at runtime and every entry point is fetched
through `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr`. Two consequences worth
being explicit about:

- A program using this module **builds and starts on a machine with no driver**.
  `vulkan.available()` returns 0 and the program degrades instead of failing to
  launch. Only the Vulkan headers are needed to build, and they are header-only.
- Device-level entry points come from `vkGetDeviceProcAddr`, which returns the
  driver's own function rather than the loader's dispatch trampoline.
- `AETHER_VULKAN_LOADER=<path>` names the loader to open instead of searching
  for one, for a machine with more than one. On Windows the DLL search takes
  `System32` before `PATH`, so a loader installed beside a driver (MSYS2's, or
  the Vulkan SDK's) loses to whatever `System32` holds; the Windows CI leg sets
  it to MSYS2's loader for lavapipe. A loader named there that does not open is
  reported, not replaced by another.

That second point is measurable rather than folklore. The two pointers differ,
and the driver entry is cheaper per call:

```
vkResetFences via loader    12.2 ns/call
vkResetFences via driver    11.6 ns/call   (5.2% less)
```

The entry points this file loads are not hand-written tables.
`aether_vulkan_dispatch.h` lists them as X-macros generated from the registry
(see below), one list per feature or extension and loading level, from the
command names in `tools/dispatch_commands.txt`.

## The Vulkan API directly: `contrib.vulkan.vk`

For anything the module above does not do, `contrib.vulkan.vk` is the API
itself, generated from the Vulkan registry
([#1506](https://github.com/aether-lang-dev/aether/issues/1506)):

```aether,fragment
import contrib.vulkan.vk

app = calloc(1, sizeof(VkApplicationInfo)) as *VkApplicationInfo
app.sType = vk.STRUCTURE_TYPE_APPLICATION_INFO
app.pApplicationName = "raw"
app.apiVersion = 1 << 22
ci = calloc(1, sizeof(VkInstanceCreateInfo)) as *VkInstanceCreateInfo
ci.sType = vk.STRUCTURE_TYPE_INSTANCE_CREATE_INFO
ci.pApplicationInfo = app as ptr
out = calloc(1, 8)
r = vk.vkCreateInstance(ci as ptr, null, out)
instance = (out as ptr[])[0]
vk.load_instance(instance)

devices, n, r2 = vk.vkEnumeratePhysicalDevices_all(instance)
defer vk.array_free(devices)
```

- **Constants** drop the `VK_` prefix (`vk.FORMAT_R8G8B8A8_UNORM`), so they
  cannot collide with the enumerators the header declares.
- **Structs and unions** are `extern struct ... @c_import`: field names and
  Aether types come from the registry, and the layout from `<vulkan/vulkan.h>`,
  so nothing depends on offsets computed by hand. A string field holds the C
  characters of the string it is given and borrows them, as the C API expects
  (docs/c-interop.md).
- **Commands** call through entry points resolved at runtime from the loader
  this module opens, so a program driving the API directly still links nothing
  and starts where there is no Vulkan; there a command returning `VkResult`
  returns `ERROR_INITIALIZATION_FAILED`. `vk.load_instance(instance)` and
  `vk.load_device(device)` point the commands at that instance's and device's
  own entry points, the driver's rather than the loader's dispatch, which is
  also how an extension command the loader does not export is reached. After
  that, a command the instance or device does not provide (one from a version
  or extension it was not created with) fails as it would with no loader
  instead of being called; a command that returns nothing then does nothing.
  The entry points are the program's, one instance and one device at a time:
  load them before other threads call the commands, pass null after
  destroying the instance or device to put the commands back on the loader's
  dispatch, and leave them there in a program that drives several devices,
  since the loader's dispatch serves any of them.
- **Strings** reach the driver as C characters, `null` as `NULL`: a name built
  at runtime works, and `vkEnumerateInstanceExtensionProperties_all(null)`
  asks for the loader's own extensions.
- **The two-call idiom** is generated. The registry records which parameter
  counts which array, so each command that fills one also has a
  `<command>_all`. It asks for the count, allocates, fills, and asks again
  while the implementation answers `INCOMPLETE`, then returns the array, its
  length and the result. Each element's `sType` is set from the registry
  first, which the `...2` queries require.

`tools/vkgen.ae` is the generator: an Aether program that streams `vk.xml`
with `std.xml` and takes a selection, a core version plus extensions, rather
than all of the registry. The committed module is Vulkan 1.3 with
`VK_KHR_surface` and `VK_KHR_swapchain`: 229 commands, 294 structs and 18
`_all` helpers, and every API constant their array members are sized by.
Platform extensions are refused, since their structs name types (`HWND`,
`Display`) that only their own headers declare. Handles are pointers, which
is what Vulkan's non-dispatchable handles are on 64-bit targets; a 32-bit
target, where they are `uint64_t`, is not supported.

```sh
contrib/vulkan/tools/regenerate.sh             # vk.xml from $VULKAN_SDK or the usual prefixes
contrib/vulkan/tools/regenerate.sh --registry /path/to/vk-1.3.204.xml
```

Both generated files record the registry release they came from, and the
committed ones come from 1.3.204, the oldest registry this repository builds
against (Ubuntu 22.04's), so every header from there on declares what they
name. Registries 1.3.204, 1.3.275, 1.4.309 and 1.4.357 give the same commands,
structs and dispatch header; later releases add enum values and aliases.
`tests/integration/vulkan_vkgen` regenerates from the installed registry and
requires the dispatch header to be identical, and the module too when the
release matches. It then builds and runs what it generated. The Linux contrib
job installs 1.3.204 and requires the releases to match, so there both files
are compared byte for byte: to regenerate, pass that registry.

What it costs: `aetherc` spends about 40 ms more on a program importing the
7,700-line module than on an empty one. A cold `ae build` of such a program
takes 6.2 s, against 5.9 s importing `contrib.vulkan` and 2.8 s for a program
importing nothing: the time is compiling `aether_vulkan.c`, which both share
and the build cache keeps. Only the commands a program calls are emitted.

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

```aether,fragment
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

The Linux CI leg leak-gates every test and example here with **LeakSanitizer**
(`make contrib-check-lsan`), not valgrind. Valgrind cannot measure this: the CI
driver is lavapipe, whose LLVM JIT it cannot follow, so one render reports
around 13,000 errors from about 1,000 contexts, all inside libvulkan and the
driver's worker threads, and the "definitely lost" total moves between runs.
Gating on that would measure Mesa rather than this module.

LSan suppresses by **module**, which is what makes the distinction possible:
`.github/scripts/lsan-contrib.supp` excludes the graphics stack by object
(`leak:libvulkan_lvp`, `leak:libLLVM`, …) and leaves every allocation this
module makes gated. A deliberate `malloc` in `aether_vulkan.c` fails the leg
with the function and line named; the driver's own 352-byte instance
allocation does not.

One piece of the gate is load-bearing and worth knowing about: the leg runs
with `.github/scripts/lsan_keep_modules.c` preloaded, a no-op `dlclose`. LSan
symbolizes at exit, and the loader unloads the ICD before then, so without it
the driver's frames resolve to `<unknown module>` and no module suppression can
match them. ASan is not used: its `memcpy` interceptor consults a shadow map
that lavapipe's JIT-mapped code sits outside of, and the process dies with a
SEGV inside LLVM's `RuntimeDyld` before the first draw.

`leaks -atExit` against a real driver remains the macOS-side check (0 leaks for
the example, the test and the C smoke test), and the eight create/draw/destroy
cycles in the test are what surface accumulation on either.

## Building against it

Importing the module is all a build needs. Its C file is declared with
`@source` and compiled into the program, and nothing is linked, which is the
point of the runtime loading. What the build needs is the Vulkan headers,
which are header-only. Where they are not on the default include path
(Homebrew on Apple silicon), name them in `aether.toml`:

```toml
[build]
cflags = "-I/opt/homebrew/include"   # or: $(pkg-config --cflags vulkan)
```

A project that still lists `contrib/vulkan/aether_vulkan.c` in
`extra_sources`, which the module asked for before `@source`, keeps building:
a file named both ways is compiled once. A glibc older than 2.34 keeps `dlopen`
in libdl, so add `link_flags = "-ldl"` there.

Headers from **1.3.204** onward work: that is what Ubuntu 22.04 ships and what
the Linux CI leg builds against, so using a symbol newer than that fails there.

| platform | headers | driver |
|---|---|---|
| Linux | `apt install libvulkan-dev` | vendor ICD, or `mesa-vulkan-drivers` for lavapipe on the CPU |
| macOS | `brew install vulkan-headers` | `brew install vulkan-loader molten-vk` |
| Windows | Vulkan SDK, or MSYS2's `mingw-w64-x86_64-vulkan-headers` | vendor ICD, or MSYS2's `mingw-w64-x86_64-mesa` for lavapipe |

## Shaders

`shaders/*.vert`, `*.frag` and `*.comp` are the GLSL sources; the `.spv` files
beside them are committed so that building needs no shader compiler. After
editing the GLSL, run `shaders/build_shaders.sh` and commit the result.
`triangle` is the built-in layout, `transform` adds a push-constant mat4,
`textured` reads a caller-described layout plus a sampler and a uniform,
`transform.comp` and `vertices.comp` are compute passes, and `pulled.vert`
reads its vertices from a storage buffer.

`tests/integration/vulkan_shaders` checks that the committed binaries are
well-formed SPIR-V and that the GLSL still compiles. It deliberately does not
byte-compare against a fresh glslang run, because the generator id and word
layout differ between glslang releases; that check would fail on version drift
rather than on a defect. What proves the pair is correct is the render test.

## Testing

`test_vulkan.ae`, `test_vulkan_resources.ae`, `test_vulkan_actors.ae`,
`test_vulkan_depth_msaa.ae`, `test_vulkan_frames.ae`,
`test_vulkan_materials.ae`, `test_vulkan_formats.ae`,
`test_vulkan_compute.ae`, `test_vulkan_present.ae` and `test_vulkan_raw.ae`
run from `make contrib-check`, along with all three examples. With no driver it prints SKIP
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

`test_vulkan_formats.ae` checks each format by a value only that format
produces: an sRGB target stores linear 0.5 as about 188 rather than 128, and
the float targets keep 2.5 and 1000.0 where the 8-bit readers clamp them to
255. A format that silently fell back to UNORM fails every one.
`test_vulkan_compute.ae` compares a compute transform with the CPU element by
element, and draws what a compute pass wrote, pulled by `gl_VertexIndex`.

`test_vulkan_present.ae` presents into a real window and reads the screen
back: the drawn colour; a resize, both through `swapchain_resize` and by the
window outgrowing the swapchain; a smaller target scaled to fit; an sRGB and a
float target shown as encoded; readback off; vsync off and back on;
minimising and restoring; and the refusals. The window comes from
`tests/support/native_window`, a test fixture that stands in for aether-ui's
`native_view` (Win32, X11 and AppKit), because the language's own CI cannot
depend on the toolkit. X11 does not shrink a minimised window, so the Linux leg
skips that one case.

`test_vulkan_raw.ae` drives `contrib.vulkan.vk` against the driver. It checks
that the constants match the header and that a nested struct field reads at
the right offset. It creates an instance and a device through their own entry
points, and requires the `_all` helpers to agree with the two calls made by
hand.

Every test passes on an NVIDIA RTX 4070 Ti and on lavapipe. The Khronos
validation layer, with synchronization validation on, reports nothing for
them.

The Linux CI leg installs lavapipe so the GPU path runs on a runner with no GPU,
and Xvfb so the presentation test has a display; it then asserts the tests did
**not** skip. A skip there would be silent loss of coverage. The Windows leg
does the same on MSYS2's lavapipe, so the Win32 surface and swapchain run
there too.

## Not here yet

Every limitation below has an issue. Nothing here is a TODO in a comment or a
plan in someone's head.

| Missing | Issue |
|---|---|
| Presenting to a Wayland surface is built but never run: no CI leg has a compositor, and the window fixture has no Wayland backend | [#2197](https://github.com/aether-lang-dev/aether/issues/2197) |
