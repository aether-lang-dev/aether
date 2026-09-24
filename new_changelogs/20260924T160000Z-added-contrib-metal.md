- **`contrib.metal`: GPU rendering, compute and presentation with Metal.** It
  has the same shape as `contrib.vulkan` and `contrib.d3d12`: offscreen targets
  in four colour formats with depth and MSAA, pipelines with vertex layouts,
  bindings and push constants, textures with mip chains, materials and
  batches, frames in flight, readback and PNG output, compute with shared
  buffers, and presentation through a `CAMetalLayer` on an NSView someone else
  owns (aether-ui's `native_view` kind 2) or a layer the program made. Shaders
  are Metal Shading Language, source compiled at runtime or a metallib; a
  stage's function is the library's one function of that kind. Resources map
  to `[[buffer(N)]]`, `[[texture(N)]]` and `[[sampler(N)]]` by binding number.
  MSL declares no threadgroup size, so `compute_set_group_size` gives it.

  The file is C and reaches Metal through the Objective-C runtime, with
  Metal, QuartzCore, Foundation and `libobjc` opened at runtime, so the module
  builds on every platform and `available()` is 0 away from macOS. A new
  macOS CI leg runs the tests on the runner's Metal device under Metal's API
  validation, and runs `contrib.vulkan`'s through MoltenVK, whose NSView
  surface is the Apple presentation path. `contrib_check.sh` takes `ONLY` to
  run a subset of its entries, and uses `gtimeout` where there is no
  `timeout`.
