- **contrib.vulkan, contrib.d3d12, contrib.metal: a draw's own pipeline,
  colour write masks, and a layered target's depth as a texture.** Outlines,
  portals and light volumes can now be drawn in one frame, and so can
  cascaded and point-light shadows.
  - **A draw's own pipeline:** `batch_set_pipeline(t, item, p)` gives a batch
    entry its own pipeline, made for the same target. One frame can then
    mark a stencil with one pipeline and draw through it with another
    (#2411).
  - **Colour write masks:** `state_color_mask(st, COLOR_*)` picks the
    channels a pipeline writes. With 0 it writes none (#2411).
  - **Layered depth:** `set_target_depth_array` and `set_target_depth_cube`
    read a layered target's depth as an array or a cube, either raw or
    through a comparison sampler. A layer never drawn reads the far plane
    (#2412).
  - **Multisampled layered targets:** `target_create_layered_ex` takes a
    sample count, and each frame resolves into its layer (#2412).
  - **Testing:**
    - an outline-style frame where one pipeline marks the stencil without
      colour and a second fills outside it;
    - a pipeline destroyed and remade at the same address, which re-records;
    - a green-and-alpha-only mask;
    - three depth layers read raw and compared;
    - six cube faces' depths read raw and compared;
    - a 4x layered target resolved layer by layer.
    These run on every backend.
