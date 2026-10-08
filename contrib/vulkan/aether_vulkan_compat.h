/* contrib/vulkan: the structs of the extensions tools/vkgen.ae spells by hand,
 * for Vulkan headers that predate them. contrib.vulkan.vk declares these
 * structs with @c_import, so the header in the translation unit must define
 * them, and the oldest headers this repository builds against (1.3.204,
 * Ubuntu 22.04's) do not. Each block is guarded by the extension's own macro,
 * so a header that has the extension supplies the definitions instead.
 * Copied from Vulkan-Headers' vulkan_core.h. */

#ifndef AETHER_VULKAN_COMPAT_H
#define AETHER_VULKAN_COMPAT_H

#include <vulkan/vulkan.h>

/* VK_EXT_mesh_shader, header 1.3.226. */
#ifndef VK_EXT_mesh_shader
typedef struct VkPhysicalDeviceMeshShaderFeaturesEXT {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           taskShader;
    VkBool32           meshShader;
    VkBool32           multiviewMeshShader;
    VkBool32           primitiveFragmentShadingRateMeshShader;
    VkBool32           meshShaderQueries;
} VkPhysicalDeviceMeshShaderFeaturesEXT;

typedef struct VkPhysicalDeviceMeshShaderPropertiesEXT {
    VkStructureType    sType;
    void*              pNext;
    uint32_t           maxTaskWorkGroupTotalCount;
    uint32_t           maxTaskWorkGroupCount[3];
    uint32_t           maxTaskWorkGroupInvocations;
    uint32_t           maxTaskWorkGroupSize[3];
    uint32_t           maxTaskPayloadSize;
    uint32_t           maxTaskSharedMemorySize;
    uint32_t           maxTaskPayloadAndSharedMemorySize;
    uint32_t           maxMeshWorkGroupTotalCount;
    uint32_t           maxMeshWorkGroupCount[3];
    uint32_t           maxMeshWorkGroupInvocations;
    uint32_t           maxMeshWorkGroupSize[3];
    uint32_t           maxMeshSharedMemorySize;
    uint32_t           maxMeshPayloadAndSharedMemorySize;
    uint32_t           maxMeshOutputMemorySize;
    uint32_t           maxMeshPayloadAndOutputMemorySize;
    uint32_t           maxMeshOutputComponents;
    uint32_t           maxMeshOutputVertices;
    uint32_t           maxMeshOutputPrimitives;
    uint32_t           maxMeshOutputLayers;
    uint32_t           maxMeshMultiviewViewCount;
    uint32_t           meshOutputPerVertexGranularity;
    uint32_t           meshOutputPerPrimitiveGranularity;
    uint32_t           maxPreferredTaskWorkGroupInvocations;
    uint32_t           maxPreferredMeshWorkGroupInvocations;
    VkBool32           prefersLocalInvocationVertexOutput;
    VkBool32           prefersLocalInvocationPrimitiveOutput;
    VkBool32           prefersCompactVertexOutput;
    VkBool32           prefersCompactPrimitiveOutput;
} VkPhysicalDeviceMeshShaderPropertiesEXT;

typedef struct VkDrawMeshTasksIndirectCommandEXT {
    uint32_t    groupCountX;
    uint32_t    groupCountY;
    uint32_t    groupCountZ;
} VkDrawMeshTasksIndirectCommandEXT;
#endif

#endif /* AETHER_VULKAN_COMPAT_H */
