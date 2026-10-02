- **contrib.vulkan, contrib.d3d12, contrib.metal: instancing, indirect
  draws, dynamic uniform offsets and GPU timing.** The GPU tier can now draw
  a crowd in one call, take its draw commands from the GPU, give every draw
  its own constants from one buffer, and say what a frame cost on the GPU.
  - **Vertex streams:** a layout can declare streams 1 to 7, per vertex or
    per instance, and `vertex_stream` feeds each from a buffer.
    `target_set_instances` and `batch_add_instanced` set the instances drawn.
    A draw is refused if a stream it reads is unbound or too short.
  - **Indirect draws:** `batch_add_indirect` reads draw commands from a
    buffer when the frame runs, so a compute pass can cull or sort them. A
    command has the same layout in all three APIs.
  - **Dynamic uniforms:** `bindings_uniform_dynamic` declares a window of one
    buffer, and `batch_set_offset` moves it for each draw.
    `uniform_offset_alignment` reports the alignment offsets need.
  - **GPU timing:** `target_set_timing`/`target_gpu_ms` and
    `compute_set_timing`/`compute_gpu_ms` report the milliseconds the newest
    frame or dispatch took on the GPU.
  - **Changed:** a layout that declares a second stream is no longer refused
    when the pipeline is made. A stream past 7, or one declared twice, is now
    refused by `layout_binding` in all three modules.
  - **Testing:** a spec per module checks each feature against pixels read
    back, on lavapipe (Linux and Windows), WARP, and macOS Metal and
    MoltenVK. The Vulkan spec is clean under the validation layer's
    synchronisation and best-practices checks. The Direct3D 12 spec is clean
    under the debug layer and GPU-based validation (#2198).
