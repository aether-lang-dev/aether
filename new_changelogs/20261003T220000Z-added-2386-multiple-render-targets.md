- **contrib.vulkan, contrib.d3d12, contrib.metal: targets with several colour
  attachments.** A deferred renderer's G-buffer is now one draw.
  - **New calls:** `target_create_mrt` makes a target with 1 to 4 colour
    attachments, each in its own format, and fragment output N writes
    attachment N. `set_target_attachment` and its `material_` form sample any
    attachment in a later pass. `pixel_value_at` reads any attachment back,
    and `target_attachments` reports the count.
  - **Behaviour:** every attachment clears to the draw's colour, resolves
    when multisampled, follows the target through a resize, and blends with
    the pipeline's state. Present and the whole-frame readers keep reading
    attachment 0 (#2386).
  - **Fixed:** two comments in contrib/d3d12 that earlier changes had left
    above the wrong functions are back above the ones they describe.
  - **Testing:** the sampling spec of each module writes three attachments
    in one draw: 8-bit, half-float and float, with an HDR value. It reads
    each one back, samples the float one in a later pass, and checks that
    every attachment follows a resize and resolves at 4x. It runs on
    lavapipe, WARP, the GPU, and macOS Metal and MoltenVK.
