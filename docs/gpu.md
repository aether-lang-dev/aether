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
bindings and push constants, textures with mip chains, materials and batches,
frames in flight, readback and PNG output, compute over shared buffers, and
swapchains over a window someone else owns. A program written against one
reads the same against the others. What differs is below: the shading
language, where resources sit in it, and the coordinate conventions.

A target feeds one vertex stream, binding 0, from the vertices reserved on it;
a layout that declares another binding is refused when the pipeline is made,
in all three, rather than drawn from a buffer that is never bound.

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
| Debugging | the Khronos validation layer | `AETHER_D3D12_DEBUG=1` or `2` | `MTL_DEBUG_LAYER=1` |

The format constants have the same names in all three, so code that names them
moves between modules unchanged. Only a program that stores the numbers
themselves would notice they differ.

A few calls exist in one module only, because they describe something the
others do not have: `d3d12.device_is_warp` and `d3d12.debug_message_count`,
`metal.device_unified_memory`, and `metal.compute_set_group_size`.

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
| Linux contrib | `contrib.vulkan`, and the generated declarations against the installed registry | lavapipe (Mesa's CPU Vulkan), with Xvfb as the display |
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
| Sampling a target's colour or depth in a later pass, 3D textures, indirect draws, dynamic uniform offsets, GPU timestamps, and a second vertex stream for per-instance data, in all three modules | [#2198](https://github.com/aether-lang-dev/aether/issues/2198) |
| Making a `VkSurfaceKHR` from a window handle on a program's own instance, for `contrib.vulkan.vk` | [#2199](https://github.com/aether-lang-dev/aether/issues/2199) |
| Running Wayland presentation in CI | [#2197](https://github.com/aether-lang-dev/aether/issues/2197) |
| `native_view` on GTK4 and AppKit, so aether-ui hands out kinds 2 to 4 | [aether-ui#208](https://github.com/aether-lang-dev/aether-ui/issues/208) |
