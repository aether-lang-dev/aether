- **`contrib.vulkan` presents to a window (#1505).** A swapchain is made
  over a window someone else owns, from the handle a toolkit hands out:
  `vulkan.swapchain_create(dev, kind, display, window, w, h)`, with the kinds
  numbered as aether-ui's `native_view_kind()` numbers them (Win32 HWND,
  NSView, X11, Wayland) plus a CAMetalLayer a program made itself. The
  language still owns no windowing; the handle is opaque.

  `vulkan.present(sc, target)` shows a target's newest frame, so everything a
  target already does (depth, MSAA, materials, frames in flight) reaches the
  screen unchanged, without the CPU waiting on the GPU. What presenting has to
  cope with is handled inside it:
  - a window that resized is rebuilt when it is next presented, even where
    the driver never reports it out of date (NVIDIA's keeps presenting into a
    minimised Win32 window at the old size);
  - a minimised window is skipped, and picked up again when restored;
  - a target of another size is scaled, and an sRGB or float target presents
    through an sRGB swapchain so linear light is encoded for display.

  Around it: `swapchain_resize`, `swapchain_set_vsync` (FIFO, or MAILBOX /
  IMMEDIATE), `target_resize`, which keeps every pipeline valid, and
  `target_set_readback(t, 0)`, which stops a presented-only target paying a
  full-frame copy to host memory every frame.

  `test_vulkan_present` presents into a real window and reads the screen
  back to prove the frame arrived. It runs on the Linux leg under Xvfb, on the
  Windows leg on MSYS2's lavapipe, and on a real GPU, with the Khronos
  validation layer's synchronization checks clean.
