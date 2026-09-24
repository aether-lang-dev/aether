- **`contrib.vulkan` renders in sRGB and float formats and writes PNGs
  (#1514).** `vulkan.target_create_format` takes `FORMAT_R8G8B8A8_UNORM`,
  `FORMAT_R8G8B8A8_SRGB`, `FORMAT_R16G16B16A16_SFLOAT` or
  `FORMAT_R32G32B32A32_SFLOAT`, checked against what the device can render
  to. `pixel_value` reads a channel at full precision, so HDR values above
  1.0 are visible; `copy_rgba8` converts any format to 8-bit RGBA; and
  `save_png` writes the frame as a PNG. `save_ppm` stays, for a file with no
  compressor in the path.

  The PNG writer is a module of its own, `contrib.png`
  (`png.encode_rgba8` / `png.write_rgba8`), built on `std.zlib` with no new
  dependency. It filters each row with whichever of the five PNG filters
  suits it best, the heuristic libpng uses. Its test decodes the output
  independently and requires every byte back.
