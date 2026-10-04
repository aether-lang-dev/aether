- **contrib.vulkan, contrib.d3d12, contrib.metal: texture formats and
  anisotropic filtering.** Masks, normal maps, HDR images and compressed
  assets can now be sampled through the shared shape.
  - **Formats:** `texture_create_format` makes a 2D texture in R8, RG8,
    RGBA8 UNORM or sRGB, RGBA16F, RGBA32F, or the BC1, BC3, BC4, BC5 and BC7
    block formats (BC1, BC3 and BC7 also in sRGB). `texture_format_supported`
    asks first.
  - **Mip chains:** a mipmapped uncompressed texture builds its chain on
    upload in its own format, with sRGB averaged as light.
    `texture_upload_level` uploads a block format's chain, or one made
    offline, level by level, and the texture binds once every level has
    pixels.
  - **Anisotropy:** the last argument filters anisotropically up to the
    device's limit, and `texture_anisotropy` reports what was granted.
  - **Testing:** each module's textures spec samples every uncompressed
    format against known values and checks each chain's last level. It
    decodes known BC1, BC4 and BC7 blocks, uploads a BC1 chain level by
    level, and shows a 16:1 squeezed stripe pattern staying sharp only when
    filtered anisotropically. It runs on lavapipe, WARP, the GPU, and macOS
    Metal and MoltenVK (#2397).
  - **Follow-up filed:** BC6H, ASTC and ETC2, signed formats, and cubes and
    arrays in other formats (#2402).
