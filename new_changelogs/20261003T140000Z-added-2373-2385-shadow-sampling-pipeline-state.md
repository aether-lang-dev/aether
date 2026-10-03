- **contrib.vulkan, contrib.d3d12, contrib.metal: shadow-map sampling and
  pipeline state.** The last two pieces a forward renderer needed from the
  GPU tier's shared shape.
  - **Comparison sampling:** `set_target_depth_compare` and its `material_`
    form read a target's depth through a comparison sampler, with a
    `COMPARE_*` op. The shader gets the fraction of the footprint that passes,
    filtered across texels where `target_depth_linear` is 1, so shadow edges
    are soft in hardware (#2373).
  - **Pipeline state:** `state_create`, `state_blend` (`BLEND_ALPHA`,
    `BLEND_PREMULTIPLIED`, `BLEND_ADDITIVE`), `state_cull` (`CULL_BACK`,
    `CULL_FRONT`, with counter-clockwise on screen front-facing in all three
    modules) and `state_depth` (the test's op, and whether it writes) describe
    a pipeline's fixed-function state. `pipeline_create_state` takes the
    description, and pipelines made without one are unchanged (#2385).
  - **Fixed:** contrib.vulkan reused a frame's recorded commands when a new
    pipeline or material had the address of a destroyed one, and submitted
    commands naming destroyed objects. That is an invalid command buffer, and
    on NVIDIA a lost device. A recorded frame now names them by a per-device
    serial.
  - **Testing:** the sampling and draws specs check each op, blend mode, cull
    mode and depth state against pixels read back. They run on lavapipe,
    WARP, the GPU, and macOS Metal and MoltenVK.
