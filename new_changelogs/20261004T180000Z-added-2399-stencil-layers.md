- **contrib.vulkan, contrib.d3d12, contrib.metal: stencil, and rendering
  into a layer.** Masks, portals and decal limits can now be drawn through
  the shared shape, and so can shadow cascades and reflection probes.
  - **Stencil:** `DEPTH_STENCIL` in place of `DEPTH` on any target create
    adds an 8-bit stencil, cleared with the depth. The stencil test is set
    with `state_stencil` (`COMPARE_*`, eight `STENCIL_*` ops, and read and
    write masks). `batch_set_stencil_ref` gives a draw its own reference.
    `COMPARE_EQUAL`, `COMPARE_NOT_EQUAL`, `COMPARE_ALWAYS` and
    `COMPARE_NEVER` work wherever a compare op does.
  - **Layered targets:** `target_create_layered` makes a target of several
    layers, or a cube's six faces. `target_set_layer` picks the one drawn,
    and `set_target_array` and `set_target_cube` sample them all.
  - **Testing:** each module's specs check:
    - a mask written by one draw limiting the next, and the stencil clearing
      every frame;
    - three layers drawn and read back through a sampler2DArray, with one
      redrawn while the others keep their contents;
    - six cube faces drawn and read back through a samplerCube.
    They run on lavapipe, WARP, the GPU, and macOS Metal and MoltenVK
    (#2399).
  - **Follow-ups filed:** a batch entry's own pipeline and colour write
    masks (#2411); a layered target's depth as a texture and multisampled
    layered targets (#2412); a lavapipe crash with D32S8 attachments (#2410).
