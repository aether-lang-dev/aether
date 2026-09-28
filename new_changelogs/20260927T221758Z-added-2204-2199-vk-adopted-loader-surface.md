- **`contrib.vulkan.vk` adopts a caller's `vkGetInstanceProcAddr` and makes a
  surface on the caller's instance.** `vk.load_instance_with(gipa, instance)`
  resolves the commands through a loader or interposer the program holds
  (NVIDIA Streamline, whose hooks DLSS and Reflex need), with a null instance
  routing `vkCreateInstance` itself through it; `vk.load_device` then takes the
  device's `vkGetDeviceProcAddr` from it, and `vk.load_device_with(gdpa, device)`
  takes one by hand. `vk.surface_create(instance, kind, display, window)` makes
  a `VkSurfaceKHR` over a toolkit's window handle (the five kinds
  `contrib.vulkan` presents to) on a program's own instance, with
  `vk.surface_extension(kind)` naming what to enable and `vk.last_error()`
  the reason for a refusal. The module also spells
  `VK_KHR_portability_enumeration` and `VK_KHR_portability_subset`.
