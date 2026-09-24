# contrib.d3d12: GPU rendering, compute and presentation with Direct3D 12

The Direct3D 12 member of Aether's GPU tier, alongside
[`contrib.vulkan`](../vulkan/README.md) and [`contrib.metal`](../metal/README.md).
The three have the same shape: a device, offscreen targets, pipelines with
vertex layouts and bindings, textures, materials, batches, frames in flight,
colour formats, readback, compute, and swapchains over a window someone else
owns. A program written against one reads the same against the others; what
differs is the shading language and the coordinate conventions.
[docs/gpu.md](../../docs/gpu.md) covers what the three share and how to pick
one.

```aether
import std.fs
import contrib.d3d12

main() {
    if d3d12.available() != 1 {
        println("no Direct3D 12: ${d3d12.last_error()}")
        return
    }
    dev = d3d12.device_create()
    defer d3d12.device_destroy(dev)
    target = d3d12.target_create(dev, 256, 256)
    defer d3d12.target_destroy(target)

    vs, vs_len, _e1 = fs.read_binary("contrib/d3d12/shaders/triangle_vs.hlsl")
    ps, ps_len, _e2 = fs.read_binary("contrib/d3d12/shaders/triangle_ps.hlsl")
    pipe = d3d12.pipeline_create(dev, target, vs, vs_len, ps, ps_len)
    defer d3d12.pipeline_destroy(pipe)

    d3d12.verts_reserve(target, 3)
    d3d12.verts_set(target, 0,  0.0,  0.7, 1.0, 0.0, 0.0)   // y points up
    d3d12.verts_set(target, 1, -0.7, -0.7, 0.0, 1.0, 0.0)
    d3d12.verts_set(target, 2,  0.7, -0.7, 0.0, 0.0, 1.0)

    d3d12.draw(target, pipe, 0.0, 0.0, 0.0, 1.0)
    d3d12.save_png(target, "triangle.png")
}
```

`example_triangle.ae` is that program in full, and `make contrib-check` runs it.

Importing the module is all a build needs. Its C file is declared with
`@source`, and nothing is linked. On other platforms it compiles to a stub,
`available()` is 0, and every call says why.

## Nothing links against Direct3D

`d3d12.dll`, `dxgi.dll` and the HLSL compiler (`d3dcompiler_47.dll`) are opened
at runtime, the same way `contrib.vulkan` opens the Vulkan loader. A program
using this module:

- **builds everywhere**, because the headers it needs are MinGW's, and away
  from Windows the implementation compiles to stubs;
- **starts everywhere**: where there is no Direct3D 12, `available()` is 0 and
  the program degrades;
- **renders on any Windows 10 or later**. The device is the first hardware
  adapter that supports feature level 11_0. Where there is none, it is WARP,
  the software rasteriser Windows ships. A machine without a GPU therefore
  still renders, which is how the Windows CI leg runs every test.
  `AETHER_D3D12_ADAPTER=warp` selects WARP on a machine that has a GPU, so the
  two can be compared; `device_is_warp(dev)` says which one a device is.

## Shaders and registers

Shaders are HLSL with entry point `main`. Pass either:

- **source text**, compiled at runtime for `vs_5_1` / `ps_5_1` / `cs_5_1`.
  A compile error comes back through `last_error()` with the compiler's own
  message and line;
- **bytecode** compiled ahead of time: anything in a DXBC container, which is
  what both `fxc` and `dxc` emit. It is recognised by its magic number and used
  as is. That covers Shader Model 6 as well, when `dxc` signed it.

Resources map to registers by **binding number**, so one `bindings_*`
description serves both backends:

| Binding declared with | Register in HLSL |
|---|---|
| `bindings_uniform(b, N)` | `cbuffer ... : register(bN)` |
| `bindings_texture(b, N)` | `Texture2D ... : register(tN)` and `SamplerState ... : register(sN)` |
| `bindings_storage(b, N)` | `RWStructuredBuffer<...> ... : register(uN)` |
| the push-constant block | `cbuffer ... : register(b0, space1)` |

A vertex attribute at `location` N is the input semantic `TEXCOORD`N. The
built-in layout (no layout passed) is `float2 : TEXCOORD0` and
`float3 : TEXCOORD1`, 20 bytes per vertex. An empty layout, with nothing
described, is a pipeline with no vertex input, whose vertex shader pulls its
data from a storage buffer by `SV_VertexID`.

The `shaders/` directory holds the HLSL counterparts of `contrib.vulkan`'s
GLSL, used by the tests: `triangle`, `transform` (a push-constant `mat4`),
`textured` (a texture and a tint uniform), `depth`, `transform_cs` and
`vertices_cs` (compute), and `pulled_vs`. `column_major` matrices in a
cbuffer take the same sixteen floats, in the same order, as a GLSL `mat4`.

## Coordinates

Direct3D's: x and y run -1 to 1 with **y pointing up** (Vulkan's points down),
and depth runs 0 to 1. A texture's v coordinate runs down from its top row, as
it does in Vulkan.

## Presenting to a window

```aether
sc = d3d12.swapchain_create(dev, d3d12.WINDOW_WIN32, null, hwnd, w, h)
defer d3d12.swapchain_destroy(sc)
// each frame:
d3d12.draw(target, pipe, 0.0, 0.0, 0.0, 1.0)
d3d12.present(sc, target)
```

The window is never this module's. `hwnd` comes from whoever owns it: aether-ui's
`native_view` hands one out as kind 1, and so does any toolkit with a Win32
backend. The swapchain uses the flip model (`FLIP_DISCARD`) with three BGRA8
back buffers.

- **Scaling and colour.** Direct3D 12 has no blit, so a present draws the
  target into the back buffer with a full-screen triangle. That draw scales a
  target of another size, and writes through an sRGB view when the target is
  sRGB or float. An sRGB target is therefore shown as stored, and a float
  target's linear light is encoded for display, exactly as `contrib.vulkan`
  presents.
- **Resizing** needs nothing from the caller: every present reads the window's
  client area and resizes the back buffers when it changed.
  `swapchain_resize` does it at once. `target_resize` gives the target the
  new size and keeps every pipeline.
- **Minimised** (no client area): presenting shows nothing and succeeds, and
  `swapchain_width()` is 0 until the window is restored.
- **Vsync** is on by default. `swapchain_set_vsync(sc, 0)` presents
  immediately, with tearing allowed where the display supports it
  (`DXGI_FEATURE_PRESENT_ALLOW_TEARING`).
- `target_set_readback(t, 0)` stops a presented-only target copying every
  frame back to host memory.

## Compute

```aether
b = d3d12.bindings_create()
d3d12.bindings_storage(b, 0)
c = d3d12.compute_create(dev, cs, cs_len, b, 4)
buf = d3d12.buffer_create(dev, 1024 * 4)
d3d12.compute_set_buffer(c, 0, buf)
d3d12.compute_push_float(c, 0, 2.0)
d3d12.dispatch(c, 16, 1, 1)                 // waits; dispatch_async does not
v = d3d12.buffer_float(buf, 7)
```

Buffers live in a custom heap the CPU sees with write-back caching. That is
the Direct3D 12 counterpart of Vulkan's host-visible, coherent memory: mapped
for their lifetime, zeroed when made, and readable without a copy once the
dispatch is done. A buffer can be a storage binding of a compute pass and of a
graphics pipeline, so a draw can pull vertices a compute pass wrote. A dispatch
is refused, with the reason named, if:
- a declared binding was never set, which on Direct3D 12 would remove the
  device rather than read garbage;
- it exceeds the 65535-a-dimension group limit.

## Textures and mipmaps

`texture_create_ex(dev, w, h, mipmapped, linear, repeat)` builds a full mip
chain on upload. Direct3D 12 has no blit to build it with, so each level is
the 2x2 box average of the one above, computed on the CPU. On even sizes that
is what a linear 2:1 blit produces. A 128x128 one-texel checkerboard drawn at
16x16 comes out as its average grey, and without the chain as single texels;
the resources test checks both.

## Threads

One device may be used from several threads. Direct3D 12's device is
free-threaded, but a few things are shared across every object on a device,
and the module takes one lock per device across all of them:
- the queue's submission order;
- the timeline fence every submission signals;
- the two shader-visible descriptor heaps, which every texture and target takes
  slots from.

A target, as with Vulkan, is drawn from one thread at a time.
`test_d3d12_actors.ae` runs two actors drawing through one device while
creating and destroying targets every frame.

## Debugging

`AETHER_D3D12_DEBUG=1` enables the Direct3D 12 debug layer before the device
is made, and `=2` adds GPU-based validation, which checks descriptor and
resource-state use as the GPU executes. The layer's messages are printed to
stderr. `debug_message_count(dev)` counts the warnings, errors and corruption
among them. One advisory is filtered out, #820: no clear value was given at
resource creation. Every draw here takes its clear colour as an argument, so
there is no single value to give, and it would repeat on every frame of a
correct program.

Every test in this directory passes on an NVIDIA RTX 4070 Ti and on WARP with
0 warnings or errors at both debug levels.

## Testing

`make contrib-check` runs `test_d3d12.ae`, `test_d3d12_resources.ae`,
`test_d3d12_compute.ae`, `test_d3d12_actors.ae`, `test_d3d12_present.ae` and
the example. Away from Windows they skip. On the Windows CI leg they render on
WARP, and a step asserts that none of them skipped. They check pixels, not
status codes:
- the triangle points up;
- readback rows come back tight, although the GPU pads them to 256 bytes;
- depth decides overlap and submission order does not;
- 4x MSAA blends edge pixels;
- an sRGB target stores linear 0.5 as about 188, and float targets keep 2.5
  and 1000.0;
- a pushed matrix mirrors the triangle, and a texture lands one texel per
  quadrant under a tint;
- two materials draw two textures in one frame, and 16-bit indices give the
  32-bit frame byte for byte;
- a compute transform matches the CPU element by element;
- presented frames read back off the screen through the window fixture in
  `tests/support/native_window`, which stands in for aether-ui's
  `native_view`.

The refusals are checked as well.
