- **`contrib.vulkan` presents to a Wayland surface in CI.** The window fixture
  the presentation tests use gained a Wayland backend (`wl_compositor` and
  `xdg_wm_base` through `libwayland-client`, opened at runtime like libX11),
  chosen with `AETHER_TEST_WINDOW_SYSTEM=wayland`, and the Linux contrib leg
  runs the presentation test against weston's headless backend and fails if
  it skipped. A Wayland client cannot read the screen back, so the cases that
  check pixels on screen skip there; each present is still checked to succeed
  and be counted, and the target read back.
