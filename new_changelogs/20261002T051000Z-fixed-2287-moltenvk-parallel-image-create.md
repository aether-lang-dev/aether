- **Vulkan: parallel render targets no longer crash on macOS.** Two
  actors that create their render targets at the same moment crashed inside
  MoltenVK's `vkCreateImage` on Apple's paravirtualized GPU, which runs the
  macOS CI. It was a segfault in `NSData getBytes`, reached from
  `MVKImagePlane::initSubresources`, and `contrib/vulkan`'s
  `example_parallel_render` hit it intermittently. Vulkan allows concurrent
  image creation on a device, so this is a driver bug. The Vulkan backend now
  serializes `vkCreateImage` on Apple; recording and submitting stay
  parallel (#2287).
