- **`vulkan.available()` never reports a provisional "no Vulkan" to another
  thread.** The probe stored "unavailable" before it had probed, so a second
  actor calling `available()` (or `device_name()`) while the first was still
  creating the probe instance was told there was no Vulkan; on arm64 a
  settled answer could also be seen before the device name was fully
  written. The probe now publishes its answer once, after it is final, with
  release/acquire ordering (#2287).
