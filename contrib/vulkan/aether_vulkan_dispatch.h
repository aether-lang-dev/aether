/* The entry points contrib/vulkan loads, GENERATED from the Vulkan registry
 * (vk.xml) by contrib/vulkan/tools/vkgen.ae from contrib/vulkan/tools/
 * dispatch_commands.txt. Do not edit: add a command to that list and run
 * contrib/vulkan/tools/regenerate.sh.
 *
 * One X-macro list per feature or extension and loading level: GLOBAL and
 * INSTANCE entry points come from vkGetInstanceProcAddr, DEVICE ones from
 * vkGetDeviceProcAddr. The registry says which is which (the first
 * parameter's type), and which version or extension brought each.
 *
 * Registry: 1.3.275. */

#ifndef AETHER_VULKAN_DISPATCH_H
#define AETHER_VULKAN_DISPATCH_H

/* GLOBAL */
#define AEVK_GLOBAL_FNS(X) \
    X(vkCreateInstance) \
    X(vkEnumerateInstanceExtensionProperties)

/* INSTANCE */
#define AEVK_INSTANCE_FNS(X) \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkCreateDevice) \
    X(vkGetDeviceProcAddr)

/* DEVICE */
#define AEVK_DEVICE_FNS(X) \
    X(vkDestroyDevice) \
    X(vkGetDeviceQueue) \
    X(vkDeviceWaitIdle) \
    X(vkQueueSubmit) \
    X(vkQueueWaitIdle) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkFreeCommandBuffers) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkResetCommandBuffer) \
    X(vkCreateFence) \
    X(vkDestroyFence) \
    X(vkResetFences) \
    X(vkWaitForFences) \
    X(vkCreateImage) \
    X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) \
    X(vkCreateImageView) \
    X(vkDestroyImageView) \
    X(vkCreateRenderPass) \
    X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) \
    X(vkDestroyFramebuffer) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkMapMemory) \
    X(vkUnmapMemory) \
    X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) \
    X(vkDestroyPipeline) \
    X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) \
    X(vkCmdSetViewport) \
    X(vkCmdSetScissor) \
    X(vkCmdDraw) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdPushConstants) \
    X(vkCmdBindIndexBuffer) \
    X(vkCmdDrawIndexed) \
    X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) \
    X(vkCmdBindDescriptorSets) \
    X(vkCreateSampler) \
    X(vkDestroySampler) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBufferToImage) \
    X(vkCmdBlitImage) \
    X(vkCmdCopyImage) \
    X(vkCreateSemaphore) \
    X(vkDestroySemaphore) \
    X(vkCreateComputePipelines) \
    X(vkCmdDispatch)

/* INSTANCE */
#define AEVK_KHR_SURFACE_FNS(X) \
    X(vkDestroySurfaceKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)

/* DEVICE */
#define AEVK_KHR_SWAPCHAIN_FNS(X) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR)

/* INSTANCE */
#define AEVK_KHR_WIN32_SURFACE_FNS(X) \
    X(vkCreateWin32SurfaceKHR)

/* INSTANCE */
#define AEVK_KHR_XLIB_SURFACE_FNS(X) \
    X(vkCreateXlibSurfaceKHR)

/* INSTANCE */
#define AEVK_KHR_WAYLAND_SURFACE_FNS(X) \
    X(vkCreateWaylandSurfaceKHR)

/* INSTANCE */
#define AEVK_EXT_METAL_SURFACE_FNS(X) \
    X(vkCreateMetalSurfaceEXT)

#endif /* AETHER_VULKAN_DISPATCH_H */
