- **contrib.vulkan, contrib.d3d12, contrib.metal: primitive topology, and a
  scissor and viewport per draw.** Debug lines, gizmos, point particles,
  UI clipping, split screen and shadow-atlas tiles are now all drawn through
  the shared shape.
  - **Topology:** `state_topology` takes `TOPOLOGY_TRIANGLES` (the default),
    `TOPOLOGY_TRIANGLE_STRIP`, `TOPOLOGY_LINES`, `TOPOLOGY_LINE_STRIP` or
    `TOPOLOGY_POINTS`. A point's size comes from `gl_PointSize` in Vulkan and
    `[[point_size]]` in Metal; Direct3D 12 draws points one pixel wide.
  - **Scissor and viewport:** `batch_set_scissor` and `batch_set_viewport`
    give a batch entry its own. Both are in pixels from the target's top left
    in every module. A scissor that runs outside the target is refused when
    the frame is drawn (#2398).
  - **Testing:** each module's draws spec checks a line, a line strip, a
    point, a triangle strip, two draws scissored to the two halves of the
    target, and a viewport that puts a full-target quad into one quarter, all
    against pixels read back. It runs on lavapipe, WARP, the GPU, and macOS
    Metal and MoltenVK.
  - **Follow-ups filed:** sampled-texture formats and anisotropic filtering
    (#2397), and stencil and rendering into a layer (#2399).
