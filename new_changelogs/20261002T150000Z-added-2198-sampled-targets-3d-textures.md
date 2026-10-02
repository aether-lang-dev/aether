- **contrib.vulkan, contrib.d3d12, contrib.metal: read what was rendered,
  and 3D textures.** The GPU tier can now feed one pass into the next, which
  is what shadow maps, post-processing and deferred lighting are built from.
  - **Sampling targets:** `set_target` and `set_target_depth`, with their
    `material_` forms, bind a target's newest frame where a texture goes, as
    its colour or as its depth (0 near to 1 far). The binding follows the
    target through resizes and frames in flight.
  - **Synchronisation:** each module moves the image between being drawn and
    being read itself: barriers in Vulkan and Direct3D 12, hazard tracking in
    Metal. A target's depth is stored after its pass only once something
    samples it.
  - **Refusals:** a draw that samples its own target, a target with no frame
    yet, and depth of a target without depth or with multisampling. Each is
    refused with the reason named.
  - **3D textures:** `texture_create_3d` makes RGBA volumes, uploaded slice
    after slice. `texture_depth` reports how many slices a texture has.
  - **Testing:** a sampling spec per module runs on lavapipe (Linux and
    Windows), WARP, and macOS Metal and MoltenVK. The Vulkan spec is clean
    under the validation layer's synchronisation and best-practices checks,
    and the Direct3D 12 spec under the debug layer (#2198).
