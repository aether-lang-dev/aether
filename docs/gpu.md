# The GPU tier

Aether reaches the GPU through three modules in `contrib/`, one per native
API:

| Module | API | Runs on |
|---|---|---|
| [`contrib.vulkan`](../contrib/vulkan/README.md) | Vulkan 1.0 and later | Linux, Windows, and macOS through MoltenVK, wherever a Vulkan driver is installed |
| [`contrib.d3d12`](../contrib/d3d12/README.md) | Direct3D 12 | Windows 10 and later, on a GPU or on WARP, the software rasteriser Windows ships |
| [`contrib.metal`](../contrib/metal/README.md) | Metal | macOS |

They have **the same shape**: the same calls, with the same names, arguments,
status codes and behaviour. Each gives a device, offscreen targets in four
colour formats with depth and multisampling, pipelines with vertex layouts,
bindings and push constants, textures with mip chains, 3D textures, cube
maps and 2D arrays, textures compute passes write, targets with several
colour attachments,
materials and batches, instanced and indirect draws, dynamic uniform offsets,
a target's colour or depth read back as a texture in a later pass, frames in
flight, GPU timing, readback and PNG output, compute over shared buffers, and
swapchains over a window someone else owns. A program written against one
reads the same against the others. What differs is below: the shading
language, where resources sit in it, and the coordinate conventions.

For anything the shared shape does not cover,
[`contrib.vulkan.vk`](../contrib/vulkan/README.md#the-vulkan-api-directly-contribvulkanvk)
is the Vulkan API itself, generated from the Khronos registry.

They are in `contrib/` rather than `std/` because each depends on a driver or
an SDK that does not ship with every system, which
[stdlib-vs-contrib.md](stdlib-vs-contrib.md) keeps out of the standard
library.

## Picking one

- **One program for every platform**: `contrib.vulkan`. It is the only one of
  the three on Linux, and it runs on Windows with any vendor driver and on
  macOS through MoltenVK.
- **Windows only, or no Vulkan driver there**: `contrib.d3d12`. Every Windows
  10 or later has Direct3D 12, and a machine without a GPU still renders on
  WARP.
- **macOS only**: `contrib.metal`. Metal ships with the OS, where Vulkan needs
  MoltenVK installed.

A program can import more than one and choose when it starts, since each
reports whether it can run instead of failing to load:

```aether,fragment
import contrib.metal
import contrib.d3d12
import contrib.vulkan

backend() -> string {
    if metal.available() == 1 { return "metal" }
    if d3d12.available() == 1 { return "d3d12" }
    if vulkan.available() == 1 { return "vulkan" }
    return "none"
}
```

## Nothing is linked

Each module opens its API at runtime: the Vulkan loader, `d3d12.dll` and
`dxgi.dll`, or Metal and the Objective-C runtime. Nothing appears on the link
line, and each module's C file is declared with `@source`, so importing the
module is all a build needs. The consequences are the same for all three:

- a program **builds on every platform**; away from the module's platform the
  implementation compiles to a stub;
- a program **starts on every platform**: where the API or a device is
  missing, `available()` is 0, `last_error()` says why, and every call returns
  `ERR_NO_LOADER` rather than crashing.

## Status codes

Every call returns one of the same codes, or a null handle with the reason in
`last_error()`:

| Code | Value | Meaning |
|---|---:|---|
| `OK` | 0 | |
| `ERR_NO_LOADER` | -1 | the API is not on this system |
| `ERR_NO_DEVICE` | -2 | the API is present, with no usable device |
| `ERR_ARG` | -3 | an impossible argument |
| `ERR_OOM` | -4 | a host or device allocation failed |
| `ERR_UNSUPPORTED` | -5 | the device cannot do what was asked |
| `ERR_SHADER` | -6 | a shader was rejected |
| `ERR_DEVICE_LOST` | -7 | the GPU failed, hung or timed out |

## What differs

| | `contrib.vulkan` | `contrib.d3d12` | `contrib.metal` |
|---|---|---|---|
| Shaders | SPIR-V | HLSL source, or DXBC | MSL source, or a metallib |
| Entry point | `main` | `main` | the library's one function of the stage's kind |
| Uniform or storage buffer at binding N | `binding = N` | `bN` / `uN` | `[[buffer(N)]]` |
| Texture at binding N | `binding = N` (combined sampler) | `tN` and `sN` | `[[texture(N)]]` and `[[sampler(N)]]` |
| Push constants | `push_constant` block | `b0, space1` | `[[buffer(8)]]` |
| Vertex attribute at location N | `location = N` | `TEXCOORD`N | `[[attribute(N)]]` |
| Compute group size | `local_size` in the shader | `[numthreads]` in the shader | `compute_set_group_size`, since MSL has none |
| y axis | points down | points up | points up |
| Depth range | 0 to 1 | 0 to 1 | 0 to 1 |
| `FORMAT_*` numbers | VkFormat | DXGI_FORMAT | VkFormat, translated where used |
| A cube map, an array, a storage texture | `samplerCube`, `sampler2DArray`, `image2D`/`image3D` with its format | `TextureCube`, `Texture2DArray`, `RWTexture2D`/`RWTexture3D` at `uN` | `texturecube`, `texture2d_array`, `texture2d`/`texture3d` with `access::write` |
| Vertex stream B | `binding = B` in the layout | input slot B | `[[buffer(16 + B)]]` |
| Instance index in the shader | `gl_InstanceIndex`, counting from the first instance | `SV_InstanceID`, counting from 0 | `[[instance_id]]`, counting from the first instance |
| Dynamic uniform offsets are multiples of | the device's `minUniformBufferOffsetAlignment` | 256 | 256 |
| An indirect command's first instance | honoured where the device has `drawIndirectFirstInstance`, and must be 0 elsewhere | honoured | honoured |
| GPU time | timestamp queries; through MoltenVK on a GPU without Metal counter sampling (a virtual machine's), a frame's queries are stamped together and read 0 | a timestamp query heap | the command buffer's `GPUStartTime` to `GPUEndTime` |
| Debugging | the Khronos validation layer | `AETHER_D3D12_DEBUG=1` or `2` | `MTL_DEBUG_LAYER=1` |

The format constants have the same names in all three, so code that names them
moves between modules unchanged. Only a program that stores the numbers
themselves would notice they differ.

A few calls exist in one module only, because they describe something the
others do not have: `d3d12.device_is_warp` and `d3d12.debug_message_count`,
`metal.device_unified_memory`, and `metal.compute_set_group_size`.

## Reading what was rendered

A target's newest frame can be bound where a texture goes, as its colour or
its depth, which is what shadow maps, post-processing and deferred lighting
are built from:

```aether,fragment
vulkan.draw(shadow, shadow_pipe, 0.0, 0.0, 0.0, 1.0)  // draw the source first
vulkan.set_target_depth(lit_pipe, 0, shadow)          // its depth, near 0 to far 1
vulkan.set_target(post_pipe, 0, scene)                // or its colour
```

The binding names the target, not one image, so it follows the target: a
resize, or a newer frame of a target with frames in flight, is what the next
draw reads. `material_set_target` and `material_set_target_depth` do the same
for a material. Each module moves the image between being drawn and being
read itself (Vulkan and Direct3D 12 with barriers, Metal by its own hazard
tracking), so nothing waits for the GPU between the passes.

Refused, in all three, with the reason named:

- a draw that samples the target it draws into;
- a target that has no frame yet;
- the depth of a target made without depth, or of a multisampled one.

Vulkan and Metal store a target's depth at the end of its pass only once
something samples it; until then the pass discards it, which is what a tiler
wants.

A shadow map reads its depth through a comparison sampler, which compares a
reference depth with each texel and returns the fraction that pass:

```aether,fragment
vulkan.set_target_depth_compare(lit_pipe, 0, shadow, vulkan.COMPARE_LESS_EQUAL)
```

The shader samples it as a `sampler2DShadow`, a `Texture2D` through a
`SamplerComparisonState` (`SampleCmp`), or a `depth2d` with
`sample_compare`. A texel passes when `reference op texel`. Where
`target_depth_linear(t)` is 1, which is every Direct3D 12 and Metal device
and most Vulkan ones, the sampler filters the result across neighbouring
texels, so a shadow's edge comes back as a fraction rather than a step.

`texture_create_3d(dev, w, h, depth, linear, repeat)` is a volume: `depth`
slices of RGBA, uploaded slice after slice and read through a 3D sampler.
`texture_depth(tex)` reports the slice count, 1 for a 2D texture.

## Several colour attachments

`target_create_mrt(dev, w, h, count, f0, f1, f2, f3, depth, samples)` makes
a target with `count` colour attachments, 1 to 4, which is a deferred
renderer's G-buffer written in one draw. Attachment N is in format fN, and
the fragment shader's output N writes it: `location = N`, `SV_TargetN` or
`[[color(N)]]`. Every attachment clears to the draw's colour and resolves
when multisampled, and a pipeline's blend state applies to each one.

```aether,fragment
gbuf = vulkan.target_create_mrt(dev, w, h, 3, vulkan.FORMAT_R8G8B8A8_UNORM,
                                vulkan.FORMAT_R16G16B16A16_SFLOAT, vulkan.FORMAT_R16G16B16A16_SFLOAT,
                                0, 1, 1)
vulkan.set_target_attachment(light_pipe, 0, gbuf, 0)   // albedo
vulkan.set_target_attachment(light_pipe, 1, gbuf, 1)   // normals
vulkan.set_target_depth(light_pipe, 2, gbuf)
```

`set_target_attachment` and its `material_` form bind attachment N where a
texture goes; `set_target` is attachment 0. `pixel_value_at(t, n, x, y,
channel)` reads any attachment back. `present`, `pixel`, `copy_rgba` and
`save_png` read attachment 0, and `target_attachments(t)` reports how many
there are.

## Cube maps, arrays, and textures compute writes

`texture_create_cube(dev, size, mipmapped, linear)` is six square faces,
uploaded +X, -X, +Y, -Y, +Z, -Z. A shader samples it with a direction, and
the face and the texel are picked the same way in all three APIs.
`texture_create_array(dev, w, h, layers, mipmapped, linear, repeat)` is
`layers` images of one size, uploaded one after another and sampled with
the layer as a coordinate. Mip chains are built per face and per layer, and
`texture_layers(tex)` reports 6, the array's layers, or 1.

`texture_create_storage(dev, w, h, depth, format)` is a texture a compute
pass writes: 2D, or 3D when `depth` is above 1, in RGBA8, RGBA16F or
RGBA32F. A pass declares it with `bindings_storage_texture` and binds it
with `compute_set_storage_texture`. A draw samples it like any texture once
the dispatch has run, and each module orders the two itself. It starts
zeroed. Draws do not take storage bindings.

```aether,fragment
img = vulkan.texture_create_storage(dev, 256, 256, 1, vulkan.FORMAT_R16G16B16A16_SFLOAT)
vulkan.bindings_storage_texture(cbinds, 0)
vulkan.compute_set_storage_texture(blur, 0, img)
vulkan.dispatch(blur, 32, 32, 1)
vulkan.set_texture(post_pipe, 0, img)
```

## Drawing many things

Binding 0 is the target's own vertices. A layout can declare streams 1 to 7
as well, each advancing per vertex or per instance, and each fed from a
buffer:

```aether,fragment
vulkan.layout_binding(lay, 0, 8, vulkan.PER_VERTEX)     // the mesh
vulkan.layout_binding(lay, 1, 20, vulkan.PER_INSTANCE)  // offset + colour each
vulkan.vertex_stream(target, 1, instances)              // a buffer_create buffer
vulkan.target_set_instances(target, 1000)
```

`batch_add_instanced(t, m, first, count, first_instance, instances)` draws a
range of instances, and a per-instance stream reads from `first_instance`.
A draw is refused if a stream the pipeline reads has no buffer, or has fewer
bytes than the vertices or instances it draws.

An indirect draw reads its commands from a buffer when the frame runs, so a
compute pass can write them, for culling or sorting on the GPU:

```aether,fragment
vulkan.batch_add_indirect(target, mat, commands, 0, 2)  // two commands at byte 0
```

A command is five 32-bit words when the target has indices: index count,
instance count, first index, vertex offset and first instance. Without
indices it is four words: vertex count, instance count, first vertex and
first instance. All three APIs lay their commands out this way, so a buffer
written for one draws on the others. The words are the GPU's to supply, and
they are not checked against the geometry, the same as in the native APIs.

A dynamic uniform is a window of one buffer whose offset each draw chooses.
One buffer then holds every draw's block, which is how an engine fills its
per-draw constants once a frame:

```aether,fragment
vulkan.bindings_uniform_dynamic(binds, 0, 64)  // a 64-byte block a draw
vulkan.set_buffer(pipe, 0, ring)
vulkan.batch_add(target, null, 0, 36)
vulkan.batch_set_offset(target, 0, 0, 0)
vulkan.batch_add(target, null, 36, 36)
vulkan.batch_set_offset(target, 1, 0, 256)
```

Each offset is a multiple of `uniform_offset_alignment(dev)`, and a draw
whose window runs past the buffer is refused. Compute passes take no dynamic
uniforms.

`target_set_timing(t, 1)` times each frame on the GPU, from its start to its
end, readback copy included, and `target_gpu_ms(t)` reports the newest frame
the host has waited for, in milliseconds. `compute_set_timing` and
`compute_gpu_ms` do the same for dispatches. Both are -1 while timing is off
and until a timed frame has finished, and a queue that keeps no time refuses
timing with `ERR_UNSUPPORTED`.

## Pipeline state

A pipeline blends, culls and tests depth the way a state description says,
built like a layout and passed to `pipeline_create_state`:

```aether,fragment
st = vulkan.state_create()
vulkan.state_blend(st, vulkan.BLEND_ALPHA)               // NONE, ALPHA, PREMULTIPLIED, ADDITIVE
vulkan.state_cull(st, vulkan.CULL_BACK)                  // NONE, BACK, FRONT
vulkan.state_depth(st, vulkan.COMPARE_LESS_EQUAL, 0)     // test, without writing
pipe = vulkan.pipeline_create_state(dev, target, vs, vl, fs, fl, layout, 0, binds, st)
vulkan.state_destroy(st)
```

A pipeline made without one, or with a fresh one, does not blend or cull,
and with depth it runs a LESS test that writes, as every pipeline did before
state existed. A face is front-facing when its corners run counter-clockwise
on screen, in all three modules, whichever way each API's y points. A depth
state on a target without depth is refused, and so is blending a format the
device cannot blend.

## Windows belong to someone else

None of the modules makes a window, and the language does not own windowing. A
swapchain is made over the native handle that whoever owns the window hands
out: aether-ui, another toolkit, or a program's own code. The kinds are
numbered as aether-ui's `native_view_kind()` numbers them, so its handle passes
straight through:

| Kind | Handle | `contrib.vulkan` | `contrib.d3d12` | `contrib.metal` |
|---|---|:---:|:---:|:---:|
| 1 `WINDOW_WIN32` | `HWND` | yes | yes | |
| 2 `WINDOW_NSVIEW` | `NSView*` | yes | | yes |
| 3 `WINDOW_X11` | `Display*` and `Window` | yes | | |
| 4 `WINDOW_WAYLAND` | `wl_display*` and `wl_surface*` | yes | | |
| 5 `WINDOW_METAL_LAYER` | `CAMetalLayer*` | yes | | yes |

```aether,fragment
sc = vulkan.swapchain_create(dev, kind, display, window, width, height)
// each frame
vulkan.draw(target, pipe, 0.0, 0.0, 0.0, 1.0)
vulkan.present(sc, target)
```

In all three, `present` scales the target to the window and encodes an sRGB or
float target for display. The swapchain follows the window's size, and a
minimised window is skipped without an error. `target_resize` keeps every
pipeline made for the target.

The tests present into real windows made by a small fixture,
`tests/support/native_window`, with Win32, X11 and AppKit backends. It stands
in for aether-ui's `native_view`, because this repository's CI cannot depend on
a toolkit that is itself built with this compiler.

## Where each is tested

| CI leg | Runs | On |
|---|---|---|
| Linux contrib | `contrib.vulkan`, and the generated declarations against the installed registry | lavapipe (Mesa's CPU Vulkan), with Xvfb as the display, and weston's headless backend for the Wayland surface |
| Windows | `contrib.vulkan` and `contrib.d3d12` | lavapipe from MSYS2, and WARP |
| macOS contrib | `contrib.metal`, and `contrib.vulkan` through MoltenVK | the runner's Metal device |

Each leg asserts that the tests ran instead of skipping, because a GPU test
that quietly skips on a machine that has the driver is coverage lost. The
exceptions are the cases the window fixture cannot do on a platform, which
each test names: reading the screen back on macOS, for example.

## Not here yet

Each gap has an issue:

| Missing | Issue |
|---|---|
| The pixels a Wayland compositor shows are not checked: the Wayland leg checks presents and the target, not the screen | [#2389](https://github.com/aether-lang-dev/aether/issues/2389) |
| `native_view` on GTK4 and AppKit, so aether-ui hands out kinds 2 to 4 | [aether-ui#208](https://github.com/aether-lang-dev/aether-ui/issues/208) |
