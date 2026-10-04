- **contrib.vulkan's present test checks the pixels on a Wayland screen.**
  The test fixture's Wayland backend reads the screen through weston's
  `weston_capture_v1`: it captures the output into a `wl_shm` buffer and finds
  the window where weston's kiosk shell centres it. The Wayland CI leg starts
  weston with the kiosk shell and `--debug`. The cases that check what reached
  the screen now run there, including the drawn frame, the resized and scaled
  frames, and the sRGB and float targets' encoding, where before they skipped
  (#2389).
