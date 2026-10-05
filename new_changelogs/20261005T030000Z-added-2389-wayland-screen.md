- **contrib.vulkan's present test checks the pixels on a Wayland screen.**
  The test fixture's Wayland backend reads the screen through what weston
  offers with `--debug`: `weston_capture_v1` (weston 12 and later) or
  `weston_screenshooter` before it. It captures the output into a `wl_shm`
  buffer and reads the window where weston centres a fullscreen window. The
  Wayland CI leg runs weston that way, with pixman rendering. The cases that
  check what reached the screen now run there, including the drawn frame,
  the resized and scaled frames, and the sRGB and float targets' encoding,
  where before they skipped (#2389).
