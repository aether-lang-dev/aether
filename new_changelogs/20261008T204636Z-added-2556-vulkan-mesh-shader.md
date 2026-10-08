- **`contrib.vulkan.vk` declares mesh shaders (#2556).** `VK_EXT_mesh_shader`
  joins the generated selection: `vkCmdDrawMeshTasksEXT`,
  `vkCmdDrawMeshTasksIndirectEXT` and `vkCmdDrawMeshTasksIndirectCountEXT`,
  loaded through the device's `vkGetDeviceProcAddr`, the feature, property and
  indirect-command structs, and the task and mesh stage, pipeline stage and
  query values. The extension postdates registry 1.3.204, which the module is
  generated from, so vkgen reads it from an excerpt of a later `vk.xml`, and
  `aether_vulkan_compat.h` defines its structs for headers that old. A new
  test draws one meshlet directly, indirectly and with an indirect count,
  skipping on a device without the extension.
