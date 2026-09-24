- **`contrib.vulkan`: a frame no longer starts writing the image while the
  previous frame is still reading it.** The render pass's incoming dependency
  waited on nothing (`TOP_OF_PIPE`, no access), so with more than one frame in
  flight a frame's attachment writes were unordered against the previous
  frame's readback copy of the same image. It now waits on earlier transfer
  reads and attachment writes. Presentation reads the image the same way, so
  every presented frame depends on this. The Khronos validation layer's
  synchronization checks are clean across the module's tests.
