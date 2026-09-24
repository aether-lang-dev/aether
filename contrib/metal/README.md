# contrib.metal: GPU rendering, compute and presentation with Metal

The Metal member of Aether's GPU tier, alongside
[`contrib.vulkan`](../vulkan/README.md) and [`contrib.d3d12`](../d3d12/README.md).
The three have the same shape: a device, offscreen targets, pipelines with
vertex layouts and bindings, textures, materials, batches, frames in flight,
colour formats, readback, compute, and swapchains over a window someone else
owns. A program written against one reads the same against the others; what
differs is the shading language and the coordinate conventions.
[docs/gpu.md](../../docs/gpu.md) covers what the three share and how to pick
one.

```aether
import std.fs
import contrib.metal

main() {
    if metal.available() != 1 {
        println("no Metal: ${metal.last_error()}")
        return
    }
    dev = metal.device_create()
    defer metal.device_destroy(dev)
    target = metal.target_create(dev, 256, 256)
    defer metal.target_destroy(target)

    vs, vs_len, _e1 = fs.read_binary("contrib/metal/shaders/triangle_vs.metal")
    fs_src, fs_len, _e2 = fs.read_binary("contrib/metal/shaders/triangle_fs.metal")
    pipe = metal.pipeline_create(dev, target, vs, vs_len, fs_src, fs_len)
    defer metal.pipeline_destroy(pipe)

    metal.verts_reserve(target, 3)
    metal.verts_set(target, 0,  0.0,  0.7, 1.0, 0.0, 0.0)   // y points up
    metal.verts_set(target, 1, -0.7, -0.7, 0.0, 1.0, 0.0)
    metal.verts_set(target, 2,  0.7, -0.7, 0.0, 0.0, 1.0)

    metal.draw(target, pipe, 0.0, 0.0, 0.0, 1.0)
    metal.save_png(target, "triangle.png")
}
```

`example_triangle.ae` is that program in full, and `make contrib-check` runs it.

Importing the module is all a build needs. Its C file is declared with
`@source`, and nothing is linked. On other platforms it compiles to a stub,
`available()` is 0, and every call says why.

## Nothing links against Metal

The file is C, and it talks to Metal through the Objective-C runtime:
`libobjc`, Foundation, QuartzCore and Metal are opened at runtime, the way
`contrib.vulkan` opens the Vulkan loader. A program using this module:

- **builds everywhere**, with no framework on the link line and no
  Objective-C compiler;
- **starts everywhere**: where there is no Metal device, `available()` is 0 and
  the program degrades;
- **renders on the system default device** (`MTLCreateSystemDefaultDevice`),
  the Apple GPU on Apple silicon and the discrete or integrated GPU on an Intel
  Mac. `device_unified_memory(dev)` says whether it shares memory with the CPU.

Every Metal enum value and struct the file passes is taken from Apple's
`metal-cpp` headers, and every Metal and QuartzCore selector it sends is one
they declare. The rest are standard NSObject, NSString, NSView and NSWindow
messages, which attach a layer to a view and read its size.

## Shaders and argument indices

Shaders are Metal Shading Language. Pass either:

- **source text**, compiled at runtime. A compile error comes back through
  `last_error()` with the compiler's own message;
- **a metallib** compiled ahead of time (`xcrun metal` then `xcrun metallib`),
  recognised by its `MTLB` magic and loaded as is.

MSL reserves no entry-point name, so a stage's function is **the library's one
function of that kind**: the vertex shader's one `vertex` function, the
fragment shader's one `fragment` function, the compute shader's one `kernel`.
A library with none or with two is refused, with the functions named.

Resources map to argument-table indices by **binding number**, so one
`bindings_*` description serves every backend:

| Binding declared with | In MSL |
|---|---|
| `bindings_uniform(b, N)` | `constant T& name [[buffer(N)]]` |
| `bindings_storage(b, N)` | `device T* name [[buffer(N)]]` |
| `bindings_texture(b, N)` | `texture2d<float> name [[texture(N)]]` and `sampler name [[sampler(N)]]` |
| the push-constant block | `constant T& name [[buffer(8)]]` |
| vertex stream B | `[[buffer(16 + B)]]`, filled by the vertex descriptor |

A vertex attribute at `location` N is `[[attribute(N)]]` of the `[[stage_in]]`
struct. The built-in layout (no layout passed) is `float2` at `[[attribute(0)]]`
and `float3` at `[[attribute(1)]]`, 20 bytes a vertex. An empty layout, with
nothing described, is a pipeline with no vertex input, whose vertex shader
pulls its data from a storage buffer by `[[vertex_id]]`.

The `shaders/` directory holds the MSL counterparts of `contrib.vulkan`'s GLSL,
used by the tests: `triangle`, `transform` (a pushed `float4x4`), `textured` (a
texture and a tint uniform), `depth`, `transform_cs` and `vertices_cs`
(compute), and `pulled_vs`. The vertex outputs carry `[[user(...)]]` names, so
any vertex shader here pairs with the fragment shader that reads those names.
A `float4x4` takes the same sixteen floats, in the same order, as a GLSL
`mat4`.

## Formats

The `FORMAT_*` constants have `contrib.vulkan`'s names and numbers. Metal keeps
pixel formats and vertex formats in two enums, and one name here serves both:
`FORMAT_R32G32B32A32_SFLOAT` is a target format and a vertex attribute format.
The module translates the number to an `MTLPixelFormat` or an `MTLVertexFormat`
where it is used. `target_format()` reports the same numbers.

| Target format | What it stores |
|---|---|
| `FORMAT_R8G8B8A8_UNORM` | 0..1 as 0..255 |
| `FORMAT_R8G8B8A8_SRGB` | linear output encoded for display: 0.5 is stored as 188 |
| `FORMAT_R16G16B16A16_SFLOAT` | half floats, not clamped |
| `FORMAT_R32G32B32A32_SFLOAT` | single floats, not clamped |

## Coordinates

Metal's: x and y run -1 to 1 with **y pointing up** (Vulkan's points down), and
depth runs 0 to 1, as in Direct3D. A texture's v coordinate runs down from its
top row.

## Presenting to a window

```aether
sc = metal.swapchain_create(dev, metal.WINDOW_NSVIEW, null, view, w, h)
defer metal.swapchain_destroy(sc)
// each frame:
metal.draw(target, pipe, 0.0, 0.0, 0.0, 1.0)
metal.present(sc, target)
```

The window is never this module's. `view` comes from whoever owns it:
aether-ui's `native_view` hands an NSView out as kind 2. The view is given a
`CAMetalLayer`, which must happen on the main thread, as layer-hosting at the
window's backing scale, so AppKit never draws into it. A program that made its
own `CAMetalLayer` passes it as `WINDOW_METAL_LAYER` (kind 5). A bare layer has
no view to follow, so it takes the size given to `swapchain_create` and
`swapchain_resize`.

- **Scaling and colour.** A present draws the target into the next drawable
  with a full-screen triangle, which scales a target of another size. The
  layer's drawables are sRGB when the target is sRGB or float, so an sRGB
  target is shown as stored and a float target's linear light is encoded for
  display, as `contrib.vulkan` and `contrib.d3d12` present.
- **Resizing** needs nothing from the caller: every present reads the view's
  size and resizes the drawables when it changed. `swapchain_resize` does it at
  once. `target_resize` gives the target the new size and keeps every pipeline.
- **Miniaturised**: presenting shows nothing and succeeds, and
  `swapchain_width()` is 0 until the window is restored.
- **Vsync** is on by default (`displaySyncEnabled`). `swapchain_set_vsync(sc, 0)`
  presents as soon as a drawable is free.
- `target_set_readback(t, 0)` stops a presented-only target copying every
  frame back to host memory.

The present is committed on the queue that rendered the frame, so Metal orders
the sampling after the target's writes, and the target's next frame after the
sampling.

## Compute

```aether
b = metal.bindings_create()
metal.bindings_storage(b, 0)
c = metal.compute_create(dev, cs, cs_len, b, 4)
metal.compute_set_group_size(c, 64, 1, 1)   // MSL does not declare it
buf = metal.buffer_create(dev, 1024 * 4)
metal.compute_set_buffer(c, 0, buf)
metal.compute_push_float(c, 0, 2.0)
metal.dispatch(c, 16, 1, 1)                 // 16 groups of 64; waits
v = metal.buffer_float(buf, 7)
```

The one call the other backends do not have is `compute_set_group_size`. GLSL
declares a kernel's group size with `local_size` and HLSL with `[numthreads]`,
and the dispatch counts groups of it. MSL declares none, so it is given here,
checked against the pipeline's `maxTotalThreadsPerThreadgroup`. A dispatch
before it is set is refused rather than run with a guessed size.

Buffers are shared (`MTLResourceStorageModeShared`): mapped for their lifetime,
zeroed when made, and readable without a copy once the dispatch is done. A
buffer can be a storage binding of a compute pass and of a graphics pipeline,
so a draw can pull vertices a compute pass wrote. A dispatch is refused, with
the reason named, when a declared binding was never set.

## Waiting, and timeouts

Every command buffer the module commits signals a semaphore from its
completion handler. `draw`, `wait_all`, `dispatch` and the readers wait on it
for at most the target's or pass's timeout (`target_set_timeout_ms`,
`compute_set_timeout_ms`, 5000 ms by default), and report a command buffer
that failed with Metal's own error text.

## Textures and mipmaps

`texture_create_ex(dev, w, h, mipmapped, linear, repeat)` uploads through a
staging buffer and, for a mipmapped texture, builds the chain on the GPU with
`generateMipmapsForTexture`. A 128x128 one-texel checkerboard drawn at 16x16
comes out as its average grey, and without the chain as single texels; the
resources test checks both.

A draw of 16-bit indices must start at an even index. Metal requires an index
buffer offset to be a multiple of 4 bytes, and the module refuses such a draw
rather than letting it read the wrong indices.

## Threads

One device may be used from several threads. Metal's device and command queue
are thread-safe. The module takes one lock per device around committing
command buffers and around making the present pass on first use. A target, as
with the other backends, is drawn from one thread at a time.
`test_metal_actors.ae` runs two actors drawing through one device while
creating and destroying targets every frame.

## Debugging

Apple's own switches apply to any Metal program. `MTL_DEBUG_LAYER=1` turns on
API validation, which stops the program at the first misuse with a message
naming it; the macOS CI leg runs these tests with it on.
`MTL_SHADER_VALIDATION=1` checks shader memory access as the GPU executes, and
fails the command buffer that broke a rule, whose error text the module then
reports.

## Testing

`make contrib-check` runs `test_metal.ae`, `test_metal_resources.ae`,
`test_metal_compute.ae`, `test_metal_actors.ae`, `test_metal_present.ae` and
the example. Away from macOS they skip. The macOS CI leg runs them on the
runner's Metal device, and a step asserts that none of them skipped, beyond
the cases below. They check pixels, not status codes, with the same checks as
the Direct3D 12 tests:
- the triangle points up;
- depth decides overlap and submission order does not;
- 4x MSAA blends edge pixels;
- an sRGB target stores linear 0.5 as about 188, and float targets keep 2.5
  and 1000.0;
- a pushed matrix mirrors the triangle, and a texture lands one texel per
  quadrant under a tint;
- two materials draw two textures in one frame, and 16-bit indices give the
  32-bit frame byte for byte;
- a compute transform matches the CPU element by element, and a draw pulls
  what a compute pass wrote.

The presentation test presents into a real window made by the fixture in
`tests/support/native_window`, which stands in for aether-ui's `native_view`.
On macOS the fixture cannot read the screen back without the screen-recording
permission, and it cannot miniaturise an AppKit window. Those cases skip there,
as `contrib.vulkan`'s do. The size, resizing, formats, readback and vsync cases
run.
