- **contrib.vulkan, contrib.d3d12, contrib.metal: cube maps, 2D arrays, and
  textures compute passes write.** Skyboxes and image-based lighting, shadow
  cascades and atlases, and compute post-processing all go through the
  shared shape now.
  - **Cube maps:** `texture_create_cube` makes six faces, uploaded +X, -X,
    +Y, -Y, +Z, -Z. A direction picks the face and the texel the same way in
    every module (#2387).
  - **2D arrays:** `texture_create_array` makes `layers` images, uploaded one
    after another. Mip chains are built per face and per layer, and
    `texture_layers` reports how many there are (#2387).
  - **Storage textures:** `texture_create_storage` makes a 2D or 3D texture in
    RGBA8, RGBA16F or RGBA32F. A compute pass writes it through
    `bindings_storage_texture` and `compute_set_storage_texture`, and draws
    then sample it. Each module orders the writes before the reads: `GENERAL`
    layout with the dispatch's barriers on Vulkan, `UNORDERED_ACCESS`
    transitions on Direct3D 12, and hazard tracking on Metal (#2388).
  - **Testing:** a textures spec per module checks seven cube directions
    against the shared face table and every array layer. It also checks the
    per-layer mip averages, and what compute passes wrote to a 2D float
    texture and a 3D RGBA8 one, all read back through draws. It runs on
    lavapipe, WARP, the GPU, and macOS Metal and MoltenVK.
