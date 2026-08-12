/* contrib/vulkan: offscreen GPU rendering for Aether (#1495, phase 1).
 * See aether_vulkan.h for the API and the runtime-loading rationale.
 */

#include "aether_vulkan.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

/* VK_KHR_portability_enumeration landed in header 1.3.216, but Ubuntu 22.04
 * still ships 1.3.204 and the module must build there. Both values are fixed
 * by the specification, and the extension is only ever enabled after the
 * loader reports it at runtime, so defining them changes nothing on a newer
 * header that already has them. */
#ifndef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
#  define VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME "VK_KHR_portability_enumeration"
#endif
#ifndef VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
#  define VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR 0x00000001
#endif

#ifdef _WIN32
#  include <windows.h>
#  define AEVK_DLOPEN(p)      ((void*)LoadLibraryA(p))
#  define AEVK_DLSYM(h, s)    ((void*)GetProcAddress((HMODULE)(h), (s)))
#  define AEVK_DLCLOSE(h)     FreeLibrary((HMODULE)(h))
#else
#  include <dlfcn.h>
#  define AEVK_DLOPEN(p)      dlopen((p), RTLD_NOW | RTLD_LOCAL)
#  define AEVK_DLSYM(h, s)    dlsym((h), (s))
#  define AEVK_DLCLOSE(h)     dlclose(h)
#endif

#if defined(_MSC_VER)
#  define AEVK_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define AEVK_THREAD_LOCAL __thread
#else
#  define AEVK_THREAD_LOCAL
#endif

/* ------------------------------------------------------------------------ */
/* Error reporting                                                           */
/* ------------------------------------------------------------------------ */

static AEVK_THREAD_LOCAL char g_err[512];

static int aevk_fail(int code, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
    return code;
}

const char* aevk_last_error(void) { return g_err; }

static void aevk_clear_error(void) { g_err[0] = '\0'; }

/* ------------------------------------------------------------------------ */
/* Runtime loader                                                            */
/* ------------------------------------------------------------------------ */

/* Names are tried in order. The bare SONAMEs come first so a properly
 * installed loader wins; the absolute paths cover package managers whose
 * prefix is not on the default search path (Homebrew on arm64 macOS is not). */
static const char* const k_loader_names[] = {
#if defined(_WIN32)
    "vulkan-1.dll",
#elif defined(__APPLE__)
    "libvulkan.1.dylib",
    "libvulkan.dylib",
    "/opt/homebrew/lib/libvulkan.1.dylib",
    "/usr/local/lib/libvulkan.1.dylib",
    "libMoltenVK.dylib",
#else
    "libvulkan.so.1",
    "libvulkan.so",
#endif
    NULL
};

#define AEVK_GLOBAL_FNS(X)          \
    X(vkCreateInstance)             \
    X(vkEnumerateInstanceExtensionProperties)

#define AEVK_INSTANCE_FNS(X)                    \
    X(vkDestroyInstance)                        \
    X(vkEnumeratePhysicalDevices)               \
    X(vkGetPhysicalDeviceProperties)            \
    X(vkGetPhysicalDeviceMemoryProperties)      \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceFormatProperties)      \
    X(vkEnumerateDeviceExtensionProperties)     \
    X(vkCreateDevice)                           \
    X(vkGetDeviceProcAddr)

/* Resolved through vkGetDeviceProcAddr, not the loader's exported symbols:
 * those go straight to the driver and skip the loader's dispatch trampoline. */
#define AEVK_DEVICE_FNS(X)          \
    X(vkDestroyDevice)              \
    X(vkGetDeviceQueue)             \
    X(vkDeviceWaitIdle)             \
    X(vkQueueSubmit)                \
    X(vkQueueWaitIdle)              \
    X(vkCreateCommandPool)          \
    X(vkDestroyCommandPool)         \
    X(vkAllocateCommandBuffers)     \
    X(vkFreeCommandBuffers)         \
    X(vkBeginCommandBuffer)         \
    X(vkEndCommandBuffer)           \
    X(vkResetCommandBuffer)         \
    X(vkCreateFence)                \
    X(vkDestroyFence)               \
    X(vkResetFences)                \
    X(vkWaitForFences)              \
    X(vkCreateImage)                \
    X(vkDestroyImage)               \
    X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory)            \
    X(vkCreateImageView)            \
    X(vkDestroyImageView)           \
    X(vkCreateRenderPass)           \
    X(vkDestroyRenderPass)          \
    X(vkCreateFramebuffer)          \
    X(vkDestroyFramebuffer)         \
    X(vkCreateBuffer)               \
    X(vkDestroyBuffer)              \
    X(vkGetBufferMemoryRequirements)\
    X(vkBindBufferMemory)           \
    X(vkAllocateMemory)             \
    X(vkFreeMemory)                 \
    X(vkMapMemory)                  \
    X(vkUnmapMemory)                \
    X(vkCreateShaderModule)         \
    X(vkDestroyShaderModule)        \
    X(vkCreatePipelineLayout)       \
    X(vkDestroyPipelineLayout)      \
    X(vkCreateGraphicsPipelines)    \
    X(vkDestroyPipeline)            \
    X(vkCmdBeginRenderPass)         \
    X(vkCmdEndRenderPass)           \
    X(vkCmdBindPipeline)            \
    X(vkCmdBindVertexBuffers)       \
    X(vkCmdSetViewport)             \
    X(vkCmdSetScissor)              \
    X(vkCmdDraw)                    \
    X(vkCmdCopyImageToBuffer)       \
    X(vkCmdPushConstants)           \
    X(vkCmdBindIndexBuffer)         \
    X(vkCmdDrawIndexed)             \
    X(vkCreateDescriptorSetLayout)  \
    X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool)       \
    X(vkDestroyDescriptorPool)      \
    X(vkAllocateDescriptorSets)     \
    X(vkUpdateDescriptorSets)       \
    X(vkCmdBindDescriptorSets)      \
    X(vkCreateSampler)              \
    X(vkDestroySampler)             \
    X(vkCmdPipelineBarrier)         \
    X(vkCmdCopyBufferToImage)

#define AEVK_DECL(name) PFN_##name name;

typedef struct {
    AEVK_GLOBAL_FNS(AEVK_DECL)
    AEVK_INSTANCE_FNS(AEVK_DECL)
} AevkInstanceApi;

typedef struct {
    AEVK_DEVICE_FNS(AEVK_DECL)
} AevkDeviceApi;

#undef AEVK_DECL

/* Written only by aevk_available()'s probe, which settles to a fixed answer;
 * a second thread racing it repeats the same work and stores the same values,
 * the pattern aether_locale_num.c uses for its cached locale handle.
 * aevk_device_create keeps its own copy of the name rather than adding a
 * second writer here. */
static void*                     g_lib;
static PFN_vkGetInstanceProcAddr g_gipa;
static int                       g_probe;      /* 0 unprobed, 1 usable, -1 not */
static char                      g_dev_name[256];

static int aevk_load_library(void) {
    if (g_lib) return AEVK_OK;
    for (int i = 0; k_loader_names[i]; i++) {
        void* h = AEVK_DLOPEN(k_loader_names[i]);
        if (!h) continue;
        PFN_vkGetInstanceProcAddr gipa =
            (PFN_vkGetInstanceProcAddr)AEVK_DLSYM(h, "vkGetInstanceProcAddr");
        if (!gipa) { AEVK_DLCLOSE(h); continue; }
        g_lib = h;
        g_gipa = gipa;
        return AEVK_OK;
    }
    return aevk_fail(AEVK_ERR_NO_LOADER,
                     "no Vulkan loader found (tried %s and friends)",
                     k_loader_names[0]);
}

/* ------------------------------------------------------------------------ */
/* Objects                                                                   */
/* ------------------------------------------------------------------------ */

struct AevkDevice {
    AevkInstanceApi  ia;
    AevkDeviceApi    da;
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    VkQueue          queue;
    uint32_t         queue_family;
    VkCommandPool    pool;
    VkPhysicalDeviceMemoryProperties mem_props;
    uint32_t         max_dim;
    char             name[256];
};

/* Caller-described vertex input. Fixed capacity rather than a growable
 * array: a pipeline with more than this many streams or attributes is past
 * the point where a description struct is the right interface. */
#define AEVK_MAX_BINDINGS 8
#define AEVK_MAX_ATTRS    16
#define AEVK_MAX_DESC     8
#define AEVK_MAX_PUSH     128   /* the guaranteed minimum every device offers */

struct AevkTarget {
    AevkDevice*    dev;
    int            width, height;
    VkImage        image;
    VkDeviceMemory image_mem;
    VkImageView    view;
    VkRenderPass   pass;
    VkFramebuffer  fb;

    VkBuffer       readback;
    VkDeviceMemory readback_mem;
    void*          readback_ptr;    /* mapped for the target's lifetime */
    VkDeviceSize   readback_size;

    VkBuffer       vbuf;
    VkDeviceMemory vbuf_mem;
    void*          vbuf_ptr;
    int            vbuf_capacity;   /* vertices the allocation can hold */
    int            vertex_count;
    int            vertex_floats;   /* floats per vertex, 5 for the built-in layout */

    VkBuffer       ibuf;
    VkDeviceMemory ibuf_mem;
    void*          ibuf_ptr;
    int            ibuf_capacity;   /* indices the allocation can hold */
    int            index_count;     /* 0 draws non-indexed */

    unsigned char  push_data[AEVK_MAX_PUSH];
    uint32_t       push_size;

    VkCommandBuffer cmd;
    VkFence         fence;

    /* What the command buffer currently holds, so a repeat draw skips
     * re-recording. */
    int             recorded;
    AevkPipeline*   rec_pipe;
    int             rec_vertices;
    float           rec_clear[4];
    /* Anything the recorded commands baked in has to take part in the reuse
     * test, or a changed transform or index list silently redraws the old
     * frame. Push bytes are compared by value, not by size alone. */
    int             rec_indices;
    uint32_t        rec_push_size;
    unsigned char   rec_push[AEVK_MAX_PUSH];
};

struct AevkLayout {
    VkVertexInputBindingDescription   binds[AEVK_MAX_BINDINGS];
    VkVertexInputAttributeDescription attrs[AEVK_MAX_ATTRS];
    uint32_t bind_count;
    uint32_t attr_count;
};

/* What a shader may read besides vertex attributes: uniform buffers and
 * sampled images, by binding number. */
struct AevkBindings {
    VkDescriptorSetLayoutBinding b[AEVK_MAX_DESC];
    uint32_t count;
};

struct AevkTexture {
    AevkDevice*    dev;
    int            width, height;
    VkImage        image;
    VkDeviceMemory mem;
    VkImageView    view;
    VkSampler      sampler;
    int            uploaded;   /* 0 until the first upload lays it out */
};

struct AevkPipeline {
    AevkDevice*      dev;
    VkShaderModule   vert, frag;
    VkPipelineLayout layout;
    VkPipeline       pipeline;

    /* Shader resources. One descriptor set per pipeline: enough for a
     * transform, a material and its textures, and the lifetime is the
     * pipeline's so nothing outlives its pool. */
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool      pool;
    VkDescriptorSet       set;
    uint32_t              push_bytes;

    struct {
        VkBuffer       buf;
        VkDeviceMemory mem;
        void*          ptr;
        VkDeviceSize   size;
    } ub[AEVK_MAX_DESC];
};

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static int aevk_find_memory(const AevkDevice* d, uint32_t type_bits,
                            VkMemoryPropertyFlags want, uint32_t* out) {
    for (uint32_t i = 0; i < d->mem_props.memoryTypeCount; i++) {
        if (!(type_bits & (1u << i))) continue;
        if ((d->mem_props.memoryTypes[i].propertyFlags & want) == want) {
            *out = i;
            return AEVK_OK;
        }
    }
    return AEVK_ERR_UNSUPPORTED;
}

static int aevk_has_ext(const VkExtensionProperties* list, uint32_t n,
                        const char* name) {
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(list[i].extensionName, name) == 0) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Instance + device                                                         */
/* ------------------------------------------------------------------------ */

static int aevk_load_instance_api(AevkInstanceApi* ia, VkInstance inst) {
#define AEVK_LOAD(name)                                                     \
    ia->name = (PFN_##name)g_gipa(inst, #name);                             \
    if (!ia->name) return aevk_fail(AEVK_ERR_NO_LOADER,                     \
                                    "loader has no %s", #name);
    AEVK_GLOBAL_FNS(AEVK_LOAD)
    AEVK_INSTANCE_FNS(AEVK_LOAD)
#undef AEVK_LOAD
    return AEVK_OK;
}

static int aevk_load_device_api(AevkDeviceApi* da, PFN_vkGetDeviceProcAddr gdpa,
                                VkDevice dev) {
#define AEVK_LOAD(name)                                                     \
    da->name = (PFN_##name)gdpa(dev, #name);                                \
    if (!da->name) return aevk_fail(AEVK_ERR_UNSUPPORTED,                   \
                                    "driver has no %s", #name);
    AEVK_DEVICE_FNS(AEVK_LOAD)
#undef AEVK_LOAD
    return AEVK_OK;
}

/* Creates an instance, enabling the portability enumeration extension when the
 * loader advertises it. Without it MoltenVK's device is invisible, because a
 * non-conformant implementation is hidden from a 1.0 application by default. */
static int aevk_create_instance(AevkInstanceApi* ia, VkInstance* out) {
    PFN_vkCreateInstance create =
        (PFN_vkCreateInstance)g_gipa(NULL, "vkCreateInstance");
    PFN_vkEnumerateInstanceExtensionProperties enum_ext =
        (PFN_vkEnumerateInstanceExtensionProperties)
            g_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!create || !enum_ext) {
        return aevk_fail(AEVK_ERR_NO_LOADER, "loader exports no vkCreateInstance");
    }

    int portability = 0;
    uint32_t n = 0;
    if (enum_ext(NULL, &n, NULL) == VK_SUCCESS && n) {
        VkExtensionProperties* props =
            (VkExtensionProperties*)calloc(n, sizeof(*props));
        if (!props) return aevk_fail(AEVK_ERR_OOM, "out of memory");
        if (enum_ext(NULL, &n, props) == VK_SUCCESS) {
            portability = aevk_has_ext(props, n,
                                       VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        }
        free(props);
    }

    const char* exts[1];
    uint32_t ext_count = 0;
    VkInstanceCreateFlags flags = 0;
    if (portability) {
        exts[ext_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "aether";
    app.applicationVersion = 1;
    app.pEngineName = "aether-contrib-vulkan";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.flags = flags;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = ext_count;
    ci.ppEnabledExtensionNames = ext_count ? exts : NULL;

    VkResult r = create(&ci, NULL, out);
    if (r != VK_SUCCESS) {
        return aevk_fail(r == VK_ERROR_OUT_OF_HOST_MEMORY ? AEVK_ERR_OOM
                                                          : AEVK_ERR_NO_DEVICE,
                         "vkCreateInstance failed (VkResult %d)", (int)r);
    }
    return aevk_load_instance_api(ia, *out);
}

/* Discrete GPU, then integrated, then anything with a graphics queue. */
static int aevk_pick_physical(AevkInstanceApi* ia, VkInstance inst,
                              VkPhysicalDevice* out_phys, uint32_t* out_family,
                              char* out_name, size_t name_len) {
    uint32_t n = 0;
    VkResult r = ia->vkEnumeratePhysicalDevices(inst, &n, NULL);
    if (r != VK_SUCCESS || n == 0) {
        return aevk_fail(AEVK_ERR_NO_DEVICE, "no Vulkan physical device present");
    }
    VkPhysicalDevice* devs = (VkPhysicalDevice*)calloc(n, sizeof(*devs));
    if (!devs) return aevk_fail(AEVK_ERR_OOM, "out of memory");
    r = ia->vkEnumeratePhysicalDevices(inst, &n, devs);
    if (r != VK_SUCCESS) {
        free(devs);
        return aevk_fail(AEVK_ERR_NO_DEVICE, "vkEnumeratePhysicalDevices failed (%d)", (int)r);
    }

    int best_rank = -1;
    VkPhysicalDevice best = VK_NULL_HANDLE;
    uint32_t best_family = 0;
    char best_name[256] = {0};

    for (uint32_t i = 0; i < n; i++) {
        uint32_t qn = 0;
        ia->vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, NULL);
        if (!qn) continue;
        VkQueueFamilyProperties* qs =
            (VkQueueFamilyProperties*)calloc(qn, sizeof(*qs));
        if (!qs) { free(devs); return aevk_fail(AEVK_ERR_OOM, "out of memory"); }
        ia->vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qs);

        int family = -1;
        for (uint32_t q = 0; q < qn; q++) {
            if (qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = (int)q; break; }
        }
        free(qs);
        if (family < 0) continue;

        VkPhysicalDeviceProperties p;
        ia->vkGetPhysicalDeviceProperties(devs[i], &p);

        int rank;
        switch (p.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   rank = 4; break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: rank = 3; break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    rank = 2; break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:            rank = 1; break;
            default:                                     rank = 0; break;
        }
        if (rank > best_rank) {
            best_rank = rank;
            best = devs[i];
            best_family = (uint32_t)family;
            snprintf(best_name, sizeof(best_name), "%s", p.deviceName);
        }
    }
    free(devs);

    if (best_rank < 0) {
        return aevk_fail(AEVK_ERR_NO_DEVICE, "no device exposes a graphics queue");
    }
    *out_phys = best;
    *out_family = best_family;
    snprintf(out_name, name_len, "%s", best_name);
    return AEVK_OK;
}

int aevk_available(void) {
    if (g_probe) return g_probe > 0;
    /* Probe by actually creating an instance and enumerating: a loader with no
     * ICD behind it exports every symbol and still cannot render. */
    g_probe = -1;
    if (aevk_load_library() != AEVK_OK) return 0;

    AevkInstanceApi ia;
    VkInstance inst = VK_NULL_HANDLE;
    if (aevk_create_instance(&ia, &inst) != AEVK_OK) return 0;

    VkPhysicalDevice phys;
    uint32_t family;
    int ok = aevk_pick_physical(&ia, inst, &phys, &family,
                                g_dev_name, sizeof(g_dev_name)) == AEVK_OK;
    ia.vkDestroyInstance(inst, NULL);
    if (ok) { g_probe = 1; aevk_clear_error(); }
    return ok;
}

const char* aevk_device_name(void) {
    if (!aevk_available()) return "";
    return g_dev_name;
}

AevkDevice* aevk_device_create(void) {
    aevk_clear_error();
    if (aevk_load_library() != AEVK_OK) return NULL;

    AevkDevice* d = (AevkDevice*)calloc(1, sizeof(*d));
    if (!d) { aevk_fail(AEVK_ERR_OOM, "out of memory"); return NULL; }

    if (aevk_create_instance(&d->ia, &d->instance) != AEVK_OK) goto fail;
    if (aevk_pick_physical(&d->ia, d->instance, &d->phys, &d->queue_family,
                           d->name, sizeof(d->name)) != AEVK_OK) goto fail;

    VkPhysicalDeviceProperties props;
    d->ia.vkGetPhysicalDeviceProperties(d->phys, &props);
    d->max_dim = props.limits.maxImageDimension2D;
    d->ia.vkGetPhysicalDeviceMemoryProperties(d->phys, &d->mem_props);

    /* VK_KHR_portability_subset must be enabled when the device advertises it,
     * or vkCreateDevice is required to fail. This is the MoltenVK path. */
    const char* dev_exts[1];
    uint32_t dev_ext_count = 0;
    uint32_t en = 0;
    if (d->ia.vkEnumerateDeviceExtensionProperties(d->phys, NULL, &en, NULL) == VK_SUCCESS && en) {
        VkExtensionProperties* eps = (VkExtensionProperties*)calloc(en, sizeof(*eps));
        if (!eps) { aevk_fail(AEVK_ERR_OOM, "out of memory"); goto fail; }
        if (d->ia.vkEnumerateDeviceExtensionProperties(d->phys, NULL, &en, eps) == VK_SUCCESS &&
            aevk_has_ext(eps, en, "VK_KHR_portability_subset")) {
            dev_exts[dev_ext_count++] = "VK_KHR_portability_subset";
        }
        free(eps);
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = d->queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = dev_ext_count;
    dci.ppEnabledExtensionNames = dev_ext_count ? dev_exts : NULL;

    VkResult r = d->ia.vkCreateDevice(d->phys, &dci, NULL, &d->device);
    if (r != VK_SUCCESS) {
        aevk_fail(r == VK_ERROR_OUT_OF_HOST_MEMORY ? AEVK_ERR_OOM : AEVK_ERR_NO_DEVICE,
                  "vkCreateDevice failed (VkResult %d)", (int)r);
        goto fail;
    }
    if (aevk_load_device_api(&d->da, d->ia.vkGetDeviceProcAddr, d->device) != AEVK_OK) {
        goto fail;
    }
    d->da.vkGetDeviceQueue(d->device, d->queue_family, 0, &d->queue);

    VkCommandPoolCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = d->queue_family;
    r = d->da.vkCreateCommandPool(d->device, &pci, NULL, &d->pool);
    if (r != VK_SUCCESS) {
        aevk_fail(AEVK_ERR_OOM, "vkCreateCommandPool failed (VkResult %d)", (int)r);
        goto fail;
    }
    return d;

fail:
    aevk_device_destroy(d);
    return NULL;
}

void aevk_device_destroy(AevkDevice* d) {
    if (!d) return;
    if (d->device) {
        if (d->da.vkDeviceWaitIdle) d->da.vkDeviceWaitIdle(d->device);
        if (d->pool && d->da.vkDestroyCommandPool) {
            d->da.vkDestroyCommandPool(d->device, d->pool, NULL);
        }
        if (d->da.vkDestroyDevice) d->da.vkDestroyDevice(d->device, NULL);
    }
    if (d->instance && d->ia.vkDestroyInstance) {
        d->ia.vkDestroyInstance(d->instance, NULL);
    }
    free(d);
}

/* ------------------------------------------------------------------------ */
/* Target                                                                    */
/* ------------------------------------------------------------------------ */

static int aevk_make_buffer(AevkDevice* d, VkDeviceSize size,
                            VkBufferUsageFlags usage,
                            VkMemoryPropertyFlags want,
                            VkBuffer* out_buf, VkDeviceMemory* out_mem) {
    VkBufferCreateInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult r = d->da.vkCreateBuffer(d->device, &bi, NULL, out_buf);
    if (r != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkCreateBuffer failed (%d)", (int)r);

    VkMemoryRequirements req;
    d->da.vkGetBufferMemoryRequirements(d->device, *out_buf, &req);

    uint32_t type = 0;
    if (aevk_find_memory(d, req.memoryTypeBits, want, &type) != AEVK_OK) {
        d->da.vkDestroyBuffer(d->device, *out_buf, NULL);
        *out_buf = VK_NULL_HANDLE;
        return aevk_fail(AEVK_ERR_UNSUPPORTED, "no memory type with the required properties");
    }

    VkMemoryAllocateInfo mi = {0};
    mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = type;
    r = d->da.vkAllocateMemory(d->device, &mi, NULL, out_mem);
    if (r != VK_SUCCESS) {
        d->da.vkDestroyBuffer(d->device, *out_buf, NULL);
        *out_buf = VK_NULL_HANDLE;
        return aevk_fail(AEVK_ERR_OOM, "vkAllocateMemory failed (%d)", (int)r);
    }
    r = d->da.vkBindBufferMemory(d->device, *out_buf, *out_mem, 0);
    if (r != VK_SUCCESS) {
        d->da.vkFreeMemory(d->device, *out_mem, NULL);
        d->da.vkDestroyBuffer(d->device, *out_buf, NULL);
        *out_mem = VK_NULL_HANDLE;
        *out_buf = VK_NULL_HANDLE;
        return aevk_fail(AEVK_ERR_OOM, "vkBindBufferMemory failed (%d)", (int)r);
    }
    return AEVK_OK;
}

AevkTarget* aevk_target_create(AevkDevice* d, int width, int height) {
    aevk_clear_error();
    if (!d) { aevk_fail(AEVK_ERR_ARG, "device is null"); return NULL; }
    if (width <= 0 || height <= 0) {
        aevk_fail(AEVK_ERR_ARG, "size must be positive, got %dx%d", width, height);
        return NULL;
    }
    if ((uint32_t)width > d->max_dim || (uint32_t)height > d->max_dim) {
        aevk_fail(AEVK_ERR_UNSUPPORTED, "%dx%d exceeds the device limit of %u",
                  width, height, d->max_dim);
        return NULL;
    }
    /* The readback size is computed in 64-bit so a large target reports
     * UNSUPPORTED from the allocator rather than wrapping to a small buffer. */
    uint64_t bytes = (uint64_t)width * (uint64_t)height * 4u;
    if (bytes > (uint64_t)SIZE_MAX) {
        aevk_fail(AEVK_ERR_UNSUPPORTED, "%dx%d does not fit in host memory", width, height);
        return NULL;
    }

    AevkTarget* t = (AevkTarget*)calloc(1, sizeof(*t));
    if (!t) { aevk_fail(AEVK_ERR_OOM, "out of memory"); return NULL; }
    t->dev = d;
    t->width = width;
    t->height = height;
    t->readback_size = (VkDeviceSize)bytes;

    VkImageCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent.width = (uint32_t)width;
    ici.extent.height = (uint32_t)height;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult r = d->da.vkCreateImage(d->device, &ici, NULL, &t->image);
    if (r != VK_SUCCESS) {
        aevk_fail(AEVK_ERR_OOM, "vkCreateImage failed (%d)", (int)r);
        goto fail;
    }

    VkMemoryRequirements req;
    d->da.vkGetImageMemoryRequirements(d->device, t->image, &req);
    uint32_t type = 0;
    if (aevk_find_memory(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                         &type) != AEVK_OK &&
        aevk_find_memory(d, req.memoryTypeBits, 0, &type) != AEVK_OK) {
        aevk_fail(AEVK_ERR_UNSUPPORTED, "no memory type for the colour image");
        goto fail;
    }
    VkMemoryAllocateInfo mi = {0};
    mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = type;
    r = d->da.vkAllocateMemory(d->device, &mi, NULL, &t->image_mem);
    if (r != VK_SUCCESS) {
        aevk_fail(AEVK_ERR_OOM, "image memory allocation failed (%d)", (int)r);
        goto fail;
    }
    r = d->da.vkBindImageMemory(d->device, t->image, t->image_mem, 0);
    if (r != VK_SUCCESS) {
        aevk_fail(AEVK_ERR_OOM, "vkBindImageMemory failed (%d)", (int)r);
        goto fail;
    }

    VkImageViewCreateInfo vci = {0};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = t->image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    r = d->da.vkCreateImageView(d->device, &vci, NULL, &t->view);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkCreateImageView failed (%d)", (int)r); goto fail; }

    /* finalLayout TRANSFER_SRC_OPTIMAL so the copy after the render pass needs
     * no pipeline barrier: the render pass's implicit transition covers it. */
    VkAttachmentDescription att = {0};
    att.format = VK_FORMAT_R8G8B8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference ref = {0};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkSubpassDependency deps[2] = {{0}, {0}};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = 0;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo rpi = {0};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 2;
    rpi.pDependencies = deps;
    r = d->da.vkCreateRenderPass(d->device, &rpi, NULL, &t->pass);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkCreateRenderPass failed (%d)", (int)r); goto fail; }

    VkFramebufferCreateInfo fci = {0};
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fci.renderPass = t->pass;
    fci.attachmentCount = 1;
    fci.pAttachments = &t->view;
    fci.width = (uint32_t)width;
    fci.height = (uint32_t)height;
    fci.layers = 1;
    r = d->da.vkCreateFramebuffer(d->device, &fci, NULL, &t->fb);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkCreateFramebuffer failed (%d)", (int)r); goto fail; }

    if (aevk_make_buffer(d, t->readback_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &t->readback, &t->readback_mem) != AEVK_OK) {
        goto fail;
    }
    /* Mapped once and left mapped: a per-frame map/unmap pair is a driver
     * round trip that buys nothing for a buffer that lives as long as this. */
    r = d->da.vkMapMemory(d->device, t->readback_mem, 0, t->readback_size, 0, &t->readback_ptr);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkMapMemory failed (%d)", (int)r); goto fail; }

    VkCommandBufferAllocateInfo cai = {0};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = d->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    r = d->da.vkAllocateCommandBuffers(d->device, &cai, &t->cmd);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkAllocateCommandBuffers failed (%d)", (int)r); goto fail; }

    VkFenceCreateInfo fnci = {0};
    fnci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    r = d->da.vkCreateFence(d->device, &fnci, NULL, &t->fence);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkCreateFence failed (%d)", (int)r); goto fail; }

    return t;

fail:
    aevk_target_destroy(t);
    return NULL;
}

void aevk_target_destroy(AevkTarget* t) {
    if (!t) return;
    AevkDevice* d = t->dev;
    if (d && d->device) {
        d->da.vkDeviceWaitIdle(d->device);
        if (t->fence)        d->da.vkDestroyFence(d->device, t->fence, NULL);
        if (t->cmd)          d->da.vkFreeCommandBuffers(d->device, d->pool, 1, &t->cmd);
        if (t->vbuf_ptr)     d->da.vkUnmapMemory(d->device, t->vbuf_mem);
        if (t->vbuf)         d->da.vkDestroyBuffer(d->device, t->vbuf, NULL);
        if (t->vbuf_mem)     d->da.vkFreeMemory(d->device, t->vbuf_mem, NULL);
        if (t->ibuf_ptr)     d->da.vkUnmapMemory(d->device, t->ibuf_mem);
        if (t->ibuf)         d->da.vkDestroyBuffer(d->device, t->ibuf, NULL);
        if (t->ibuf_mem)     d->da.vkFreeMemory(d->device, t->ibuf_mem, NULL);
        if (t->readback_ptr) d->da.vkUnmapMemory(d->device, t->readback_mem);
        if (t->readback)     d->da.vkDestroyBuffer(d->device, t->readback, NULL);
        if (t->readback_mem) d->da.vkFreeMemory(d->device, t->readback_mem, NULL);
        if (t->fb)           d->da.vkDestroyFramebuffer(d->device, t->fb, NULL);
        if (t->pass)         d->da.vkDestroyRenderPass(d->device, t->pass, NULL);
        if (t->view)         d->da.vkDestroyImageView(d->device, t->view, NULL);
        if (t->image)        d->da.vkDestroyImage(d->device, t->image, NULL);
        if (t->image_mem)    d->da.vkFreeMemory(d->device, t->image_mem, NULL);
    }
    free(t);
}

int aevk_target_width(const AevkTarget* t)  { return t ? t->width  : 0; }
int aevk_target_height(const AevkTarget* t) { return t ? t->height : 0; }
size_t aevk_rgba_size(const AevkTarget* t)  { return t ? (size_t)t->readback_size : 0; }

/* ------------------------------------------------------------------------ */
/* Pipeline                                                                  */
/* ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ */
/* Vertex layouts and resource bindings                                      */
/* ------------------------------------------------------------------------ */

AevkLayout* aevk_layout_create(void) {
    aevk_clear_error();
    AevkLayout* l = (AevkLayout*)calloc(1, sizeof(*l));
    if (!l) { aevk_fail(AEVK_ERR_OOM, "out of memory"); return NULL; }
    return l;
}

void aevk_layout_destroy(AevkLayout* l) { free(l); }

int aevk_layout_binding(AevkLayout* l, int binding, int stride, int per_instance) {
    aevk_clear_error();
    if (!l) return aevk_fail(AEVK_ERR_ARG, "layout is null");
    if (binding < 0 || stride <= 0) {
        return aevk_fail(AEVK_ERR_ARG, "binding %d stride %d is not a stream", binding, stride);
    }
    if (l->bind_count >= AEVK_MAX_BINDINGS) {
        return aevk_fail(AEVK_ERR_ARG, "at most %d vertex bindings", AEVK_MAX_BINDINGS);
    }
    VkVertexInputBindingDescription* b = &l->binds[l->bind_count++];
    b->binding = (uint32_t)binding;
    b->stride = (uint32_t)stride;
    b->inputRate = per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE
                                : VK_VERTEX_INPUT_RATE_VERTEX;
    return AEVK_OK;
}

int aevk_layout_attr(AevkLayout* l, int location, int binding, int format, int offset) {
    aevk_clear_error();
    if (!l) return aevk_fail(AEVK_ERR_ARG, "layout is null");
    if (location < 0 || binding < 0 || offset < 0) {
        return aevk_fail(AEVK_ERR_ARG, "location, binding and offset must not be negative");
    }
    if (format <= 0) return aevk_fail(AEVK_ERR_ARG, "format %d is not a VkFormat", format);
    if (l->attr_count >= AEVK_MAX_ATTRS) {
        return aevk_fail(AEVK_ERR_ARG, "at most %d vertex attributes", AEVK_MAX_ATTRS);
    }
    VkVertexInputAttributeDescription* a = &l->attrs[l->attr_count++];
    a->location = (uint32_t)location;
    a->binding = (uint32_t)binding;
    a->format = (VkFormat)format;
    a->offset = (uint32_t)offset;
    return AEVK_OK;
}

AevkBindings* aevk_bindings_create(void) {
    aevk_clear_error();
    AevkBindings* b = (AevkBindings*)calloc(1, sizeof(*b));
    if (!b) { aevk_fail(AEVK_ERR_OOM, "out of memory"); return NULL; }
    return b;
}

void aevk_bindings_destroy(AevkBindings* b) { free(b); }

static int aevk_bindings_add(AevkBindings* b, int binding, VkDescriptorType type) {
    if (!b) return aevk_fail(AEVK_ERR_ARG, "bindings is null");
    if (binding < 0 || binding >= AEVK_MAX_DESC) {
        return aevk_fail(AEVK_ERR_ARG, "binding must be 0..%d", AEVK_MAX_DESC - 1);
    }
    for (uint32_t i = 0; i < b->count; i++) {
        if (b->b[i].binding == (uint32_t)binding) {
            return aevk_fail(AEVK_ERR_ARG, "binding %d is already declared", binding);
        }
    }
    if (b->count >= AEVK_MAX_DESC) {
        return aevk_fail(AEVK_ERR_ARG, "at most %d bindings", AEVK_MAX_DESC);
    }
    VkDescriptorSetLayoutBinding* d = &b->b[b->count++];
    d->binding = (uint32_t)binding;
    d->descriptorType = type;
    d->descriptorCount = 1;
    /* Visible to both stages: which one reads it is the shader's business,
     * and a mismatch here is a validation error the caller cannot see. */
    d->stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    return AEVK_OK;
}

int aevk_bindings_uniform(AevkBindings* b, int binding) {
    aevk_clear_error();
    return aevk_bindings_add(b, binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
}

int aevk_bindings_texture(AevkBindings* b, int binding) {
    aevk_clear_error();
    return aevk_bindings_add(b, binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
}

/* ------------------------------------------------------------------------ */
/* Textures                                                                  */
/* ------------------------------------------------------------------------ */

/* Runs one command buffer to completion on the device queue. Used by the
 * texture upload path, which has to transition layouts and copy before any
 * draw can sample the image. */
static int aevk_run_once(AevkDevice* d, VkCommandBuffer* out_cmd) {
    VkCommandBufferAllocateInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = d->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkResult r = d->da.vkAllocateCommandBuffers(d->device, &ai, out_cmd);
    if (r != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkAllocateCommandBuffers failed (%d)", (int)r);

    VkCommandBufferBeginInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = d->da.vkBeginCommandBuffer(*out_cmd, &bi);
    if (r != VK_SUCCESS) {
        d->da.vkFreeCommandBuffers(d->device, d->pool, 1, out_cmd);
        *out_cmd = VK_NULL_HANDLE;
        return aevk_fail(AEVK_ERR_OOM, "vkBeginCommandBuffer failed (%d)", (int)r);
    }
    return AEVK_OK;
}

static int aevk_submit_once(AevkDevice* d, VkCommandBuffer cmd) {
    VkResult r = d->da.vkEndCommandBuffer(cmd);
    if (r == VK_SUCCESS) {
        VkSubmitInfo si = {0};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        r = d->da.vkQueueSubmit(d->queue, 1, &si, VK_NULL_HANDLE);
        if (r == VK_SUCCESS) r = d->da.vkQueueWaitIdle(d->queue);
    }
    d->da.vkFreeCommandBuffers(d->device, d->pool, 1, &cmd);
    if (r != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "one-shot submit failed (%d)", (int)r);
    return AEVK_OK;
}

static void aevk_image_barrier(AevkDevice* d, VkCommandBuffer cmd, VkImage img,
                               VkImageLayout from, VkImageLayout to,
                               VkAccessFlags src_access, VkAccessFlags dst_access,
                               VkPipelineStageFlags src_stage,
                               VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {0};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    d->da.vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

AevkTexture* aevk_texture_create(AevkDevice* d, int width, int height) {
    aevk_clear_error();
    if (!d) { aevk_fail(AEVK_ERR_ARG, "device is null"); return NULL; }
    if (width <= 0 || height <= 0) {
        aevk_fail(AEVK_ERR_ARG, "texture size %dx%d is not positive", width, height);
        return NULL;
    }
    if ((uint32_t)width > d->max_dim || (uint32_t)height > d->max_dim) {
        aevk_fail(AEVK_ERR_UNSUPPORTED, "texture %dx%d exceeds the device limit of %u",
                  width, height, d->max_dim);
        return NULL;
    }

    AevkTexture* tex = (AevkTexture*)calloc(1, sizeof(*tex));
    if (!tex) { aevk_fail(AEVK_ERR_OOM, "out of memory"); return NULL; }
    tex->dev = d;
    tex->width = width;
    tex->height = height;

    VkImageCreateInfo ii = {0};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width = (uint32_t)width;
    ii.extent.height = (uint32_t)height;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = d->da.vkCreateImage(d->device, &ii, NULL, &tex->image);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkCreateImage failed (%d)", (int)r); goto fail; }

    VkMemoryRequirements req;
    d->da.vkGetImageMemoryRequirements(d->device, tex->image, &req);
    uint32_t type = 0;
    if (aevk_find_memory(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) != AEVK_OK) {
        aevk_fail(AEVK_ERR_UNSUPPORTED, "no device-local memory for the texture");
        goto fail;
    }
    VkMemoryAllocateInfo mi = {0};
    mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = type;
    r = d->da.vkAllocateMemory(d->device, &mi, NULL, &tex->mem);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkAllocateMemory failed (%d)", (int)r); goto fail; }
    r = d->da.vkBindImageMemory(d->device, tex->image, tex->mem, 0);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkBindImageMemory failed (%d)", (int)r); goto fail; }

    VkImageViewCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = tex->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    r = d->da.vkCreateImageView(d->device, &vi, NULL, &tex->view);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkCreateImageView failed (%d)", (int)r); goto fail; }

    VkSamplerCreateInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    r = d->da.vkCreateSampler(d->device, &si, NULL, &tex->sampler);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkCreateSampler failed (%d)", (int)r); goto fail; }

    return tex;
fail:
    aevk_texture_destroy(tex);
    return NULL;
}

void aevk_texture_destroy(AevkTexture* tex) {
    if (!tex) return;
    AevkDevice* d = tex->dev;
    if (d) {
        if (tex->sampler) d->da.vkDestroySampler(d->device, tex->sampler, NULL);
        if (tex->view)    d->da.vkDestroyImageView(d->device, tex->view, NULL);
        if (tex->image)   d->da.vkDestroyImage(d->device, tex->image, NULL);
        if (tex->mem)     d->da.vkFreeMemory(d->device, tex->mem, NULL);
    }
    free(tex);
}

int aevk_texture_upload(AevkTexture* tex, const void* rgba, size_t len) {
    aevk_clear_error();
    if (!tex || !rgba) return aevk_fail(AEVK_ERR_ARG, "texture or pixel data is null");
    size_t need = (size_t)tex->width * (size_t)tex->height * 4u;
    if (len < need) {
        return aevk_fail(AEVK_ERR_ARG, "need %zu bytes for %dx%d RGBA, got %zu",
                         need, tex->width, tex->height, len);
    }
    AevkDevice* d = tex->dev;

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    int rc = aevk_make_buffer(d, (VkDeviceSize)need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              &staging, &staging_mem);
    if (rc != AEVK_OK) return rc;

    void* map = NULL;
    VkResult r = d->da.vkMapMemory(d->device, staging_mem, 0, (VkDeviceSize)need, 0, &map);
    if (r != VK_SUCCESS) {
        d->da.vkDestroyBuffer(d->device, staging, NULL);
        d->da.vkFreeMemory(d->device, staging_mem, NULL);
        return aevk_fail(AEVK_ERR_OOM, "vkMapMemory failed (%d)", (int)r);
    }
    memcpy(map, rgba, need);
    d->da.vkUnmapMemory(d->device, staging_mem);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    rc = aevk_run_once(d, &cmd);
    if (rc == AEVK_OK) {
        aevk_image_barrier(d, cmd, tex->image,
                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           0, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkBufferImageCopy copy = {0};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = (uint32_t)tex->width;
        copy.imageExtent.height = (uint32_t)tex->height;
        copy.imageExtent.depth = 1;
        d->da.vkCmdCopyBufferToImage(cmd, staging, tex->image,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        aevk_image_barrier(d, cmd, tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        rc = aevk_submit_once(d, cmd);
    }

    d->da.vkDestroyBuffer(d->device, staging, NULL);
    d->da.vkFreeMemory(d->device, staging_mem, NULL);
    if (rc == AEVK_OK) tex->uploaded = 1;
    return rc;
}

AevkPipeline* aevk_pipeline_create(AevkDevice* d, AevkTarget* t,
                                   const void* vert_spv, size_t vert_len,
                                   const void* frag_spv, size_t frag_len) {
    return aevk_pipeline_create_ex(d, t, vert_spv, vert_len, frag_spv, frag_len,
                                   NULL, 0, NULL);
}

AevkPipeline* aevk_pipeline_create_ex(AevkDevice* d, AevkTarget* t,
                                      const void* vert_spv, size_t vert_len,
                                      const void* frag_spv, size_t frag_len,
                                      const AevkLayout* layout,
                                      int push_bytes,
                                      const AevkBindings* bindings) {
    aevk_clear_error();
    if (!d || !t) { aevk_fail(AEVK_ERR_ARG, "device or target is null"); return NULL; }
    if (!vert_spv || !frag_spv) { aevk_fail(AEVK_ERR_ARG, "shader bytes are null"); return NULL; }
    if (vert_len == 0 || frag_len == 0 || (vert_len % 4) || (frag_len % 4)) {
        aevk_fail(AEVK_ERR_SHADER,
                  "SPIR-V length must be a non-zero multiple of 4 (got %zu and %zu)",
                  vert_len, frag_len);
        return NULL;
    }

    if (push_bytes < 0 || push_bytes > AEVK_MAX_PUSH || (push_bytes % 4)) {
        aevk_fail(AEVK_ERR_ARG,
                  "push constant block must be 0..%d bytes and a multiple of 4 (got %d)",
                  AEVK_MAX_PUSH, push_bytes);
        return NULL;
    }

    AevkPipeline* p = (AevkPipeline*)calloc(1, sizeof(*p));
    if (!p) { aevk_fail(AEVK_ERR_OOM, "out of memory"); return NULL; }
    p->dev = d;

    VkShaderModuleCreateInfo smi = {0};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = vert_len;
    smi.pCode = (const uint32_t*)vert_spv;
    VkResult r = d->da.vkCreateShaderModule(d->device, &smi, NULL, &p->vert);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_SHADER, "vertex SPIR-V rejected (%d)", (int)r); goto fail; }

    smi.codeSize = frag_len;
    smi.pCode = (const uint32_t*)frag_spv;
    r = d->da.vkCreateShaderModule(d->device, &smi, NULL, &p->frag);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_SHADER, "fragment SPIR-V rejected (%d)", (int)r); goto fail; }

    /* Descriptor set layout, pool and set, when the caller declared any
     * shader resources. Pool sizes are counted per descriptor type so a
     * set with two uniforms and a texture allocates exactly that. */
    if (bindings && bindings->count > 0) {
        VkDescriptorSetLayoutCreateInfo dli = {0};
        dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dli.bindingCount = bindings->count;
        dli.pBindings = bindings->b;
        r = d->da.vkCreateDescriptorSetLayout(d->device, &dli, NULL, &p->set_layout);
        if (r != VK_SUCCESS) {
            aevk_fail(AEVK_ERR_OOM, "vkCreateDescriptorSetLayout failed (%d)", (int)r);
            goto fail;
        }

        VkDescriptorPoolSize sizes[2] = {{0}, {0}};
        uint32_t nsizes = 0, n_ub = 0, n_img = 0;
        for (uint32_t i = 0; i < bindings->count; i++) {
            if (bindings->b[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) n_ub++;
            else n_img++;
        }
        if (n_ub)  { sizes[nsizes].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                     sizes[nsizes++].descriptorCount = n_ub; }
        if (n_img) { sizes[nsizes].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                     sizes[nsizes++].descriptorCount = n_img; }

        VkDescriptorPoolCreateInfo dpi = {0};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = 1;
        dpi.poolSizeCount = nsizes;
        dpi.pPoolSizes = sizes;
        r = d->da.vkCreateDescriptorPool(d->device, &dpi, NULL, &p->pool);
        if (r != VK_SUCCESS) {
            aevk_fail(AEVK_ERR_OOM, "vkCreateDescriptorPool failed (%d)", (int)r);
            goto fail;
        }

        VkDescriptorSetAllocateInfo dsi = {0};
        dsi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsi.descriptorPool = p->pool;
        dsi.descriptorSetCount = 1;
        dsi.pSetLayouts = &p->set_layout;
        r = d->da.vkAllocateDescriptorSets(d->device, &dsi, &p->set);
        if (r != VK_SUCCESS) {
            aevk_fail(AEVK_ERR_OOM, "vkAllocateDescriptorSets failed (%d)", (int)r);
            goto fail;
        }
    }

    VkPushConstantRange pcr = {0};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = (uint32_t)push_bytes;
    p->push_bytes = (uint32_t)push_bytes;

    VkPipelineLayoutCreateInfo pli = {0};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (p->set_layout) {
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &p->set_layout;
    }
    if (push_bytes > 0) {
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
    }
    r = d->da.vkCreatePipelineLayout(d->device, &pli, NULL, &p->layout);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkCreatePipelineLayout failed (%d)", (int)r); goto fail; }

    VkPipelineShaderStageCreateInfo stages[2] = {{0}, {0}};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = p->vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = p->frag;
    stages[1].pName = "main";

    /* Built-in layout when the caller described none: one interleaved
     * stream of vec2 position and vec3 colour, which is what phase 1
     * shipped and what the existing shaders expect. */
    VkVertexInputBindingDescription bind = {0};
    bind.binding = 0;
    bind.stride = 5 * sizeof(float);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2] = {{0}, {0}};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = 0;
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = 2 * sizeof(float);

    VkPipelineVertexInputStateCreateInfo vin = {0};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (layout && layout->bind_count > 0) {
        vin.vertexBindingDescriptionCount = layout->bind_count;
        vin.pVertexBindingDescriptions = layout->binds;
        vin.vertexAttributeDescriptionCount = layout->attr_count;
        vin.pVertexAttributeDescriptions = layout->attrs;
    } else {
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &bind;
        vin.vertexAttributeDescriptionCount = 2;
        vin.pVertexAttributeDescriptions = attrs;
    }

    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    /* Viewport and scissor are dynamic so one pipeline serves every target
     * size; a resize costs a re-record, not a pipeline rebuild. */
    VkViewport vp = {0};
    VkRect2D sc = {{0, 0}, {(uint32_t)t->width, (uint32_t)t->height}};
    VkPipelineViewportStateCreateInfo vps = {0};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;

    VkDynamicState dyn_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {0};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {0};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gpi = {0};
    gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vin;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vps;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cb;
    gpi.pDynamicState = &dyn;
    gpi.layout = p->layout;
    gpi.renderPass = t->pass;
    gpi.subpass = 0;

    r = d->da.vkCreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &gpi, NULL, &p->pipeline);
    if (r != VK_SUCCESS) {
        aevk_fail(r == VK_ERROR_OUT_OF_HOST_MEMORY ? AEVK_ERR_OOM : AEVK_ERR_SHADER,
                  "vkCreateGraphicsPipelines failed (%d)", (int)r);
        goto fail;
    }
    return p;

fail:
    aevk_pipeline_destroy(p);
    return NULL;
}

void aevk_pipeline_destroy(AevkPipeline* p) {
    if (!p) return;
    AevkDevice* d = p->dev;
    if (d && d->device) {
        d->da.vkDeviceWaitIdle(d->device);
        if (p->pipeline) d->da.vkDestroyPipeline(d->device, p->pipeline, NULL);
        if (p->layout)   d->da.vkDestroyPipelineLayout(d->device, p->layout, NULL);
        if (p->frag)     d->da.vkDestroyShaderModule(d->device, p->frag, NULL);
        if (p->vert)     d->da.vkDestroyShaderModule(d->device, p->vert, NULL);
        /* Sets are freed with their pool; the layout outlives neither. */
        if (p->pool)       d->da.vkDestroyDescriptorPool(d->device, p->pool, NULL);
        if (p->set_layout) d->da.vkDestroyDescriptorSetLayout(d->device, p->set_layout, NULL);
        for (int i = 0; i < AEVK_MAX_DESC; i++) {
            if (p->ub[i].ptr) d->da.vkUnmapMemory(d->device, p->ub[i].mem);
            if (p->ub[i].buf) d->da.vkDestroyBuffer(d->device, p->ub[i].buf, NULL);
            if (p->ub[i].mem) d->da.vkFreeMemory(d->device, p->ub[i].mem, NULL);
        }
    }
    free(p);
}

/* ------------------------------------------------------------------------ */
/* Shader resources: uniform buffers, textures, push constants              */
/* ------------------------------------------------------------------------ */

int aevk_pipeline_set_uniform(AevkPipeline* p, int binding,
                              const void* data, size_t len) {
    aevk_clear_error();
    if (!p || !data) return aevk_fail(AEVK_ERR_ARG, "pipeline or data is null");
    if (binding < 0 || binding >= AEVK_MAX_DESC) {
        return aevk_fail(AEVK_ERR_ARG, "binding must be 0..%d", AEVK_MAX_DESC - 1);
    }
    if (len == 0) return aevk_fail(AEVK_ERR_ARG, "uniform data is empty");
    if (!p->set) {
        return aevk_fail(AEVK_ERR_ARG,
                         "pipeline was created without bindings, so it has no descriptor set");
    }
    AevkDevice* d = p->dev;

    /* Reuse the buffer while it is big enough; a uniform that changes every
     * frame must not allocate every frame. */
    if (p->ub[binding].buf && p->ub[binding].size < (VkDeviceSize)len) {
        if (p->ub[binding].ptr) d->da.vkUnmapMemory(d->device, p->ub[binding].mem);
        d->da.vkDestroyBuffer(d->device, p->ub[binding].buf, NULL);
        d->da.vkFreeMemory(d->device, p->ub[binding].mem, NULL);
        p->ub[binding].buf = VK_NULL_HANDLE;
        p->ub[binding].mem = VK_NULL_HANDLE;
        p->ub[binding].ptr = NULL;
        p->ub[binding].size = 0;
    }

    if (!p->ub[binding].buf) {
        int rc = aevk_make_buffer(d, (VkDeviceSize)len, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  &p->ub[binding].buf, &p->ub[binding].mem);
        if (rc != AEVK_OK) return rc;
        VkResult r = d->da.vkMapMemory(d->device, p->ub[binding].mem, 0,
                                       (VkDeviceSize)len, 0, &p->ub[binding].ptr);
        if (r != VK_SUCCESS) {
            d->da.vkDestroyBuffer(d->device, p->ub[binding].buf, NULL);
            d->da.vkFreeMemory(d->device, p->ub[binding].mem, NULL);
            p->ub[binding].buf = VK_NULL_HANDLE;
            p->ub[binding].mem = VK_NULL_HANDLE;
            return aevk_fail(AEVK_ERR_OOM, "vkMapMemory failed (%d)", (int)r);
        }
        p->ub[binding].size = (VkDeviceSize)len;

        VkDescriptorBufferInfo bi = {0};
        bi.buffer = p->ub[binding].buf;
        bi.range = (VkDeviceSize)len;
        VkWriteDescriptorSet w = {0};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = p->set;
        w.dstBinding = (uint32_t)binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &bi;
        d->da.vkUpdateDescriptorSets(d->device, 1, &w, 0, NULL);
    }

    /* Host-coherent, so the write is visible without a flush. The descriptor
     * already points at this buffer, so no set update is needed per frame. */
    memcpy(p->ub[binding].ptr, data, len);
    return AEVK_OK;
}

int aevk_pipeline_set_texture(AevkPipeline* p, int binding, AevkTexture* tex) {
    aevk_clear_error();
    if (!p || !tex) return aevk_fail(AEVK_ERR_ARG, "pipeline or texture is null");
    if (binding < 0 || binding >= AEVK_MAX_DESC) {
        return aevk_fail(AEVK_ERR_ARG, "binding must be 0..%d", AEVK_MAX_DESC - 1);
    }
    if (!p->set) {
        return aevk_fail(AEVK_ERR_ARG,
                         "pipeline was created without bindings, so it has no descriptor set");
    }
    if (!tex->uploaded) {
        /* Sampling an image still in UNDEFINED layout is undefined behaviour
         * and reads as garbage, so refuse rather than render nonsense. */
        return aevk_fail(AEVK_ERR_ARG, "texture has no pixels yet, upload before binding");
    }
    AevkDevice* d = p->dev;

    VkDescriptorImageInfo ii = {0};
    ii.sampler = tex->sampler;
    ii.imageView = tex->view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w = {0};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = p->set;
    w.dstBinding = (uint32_t)binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    d->da.vkUpdateDescriptorSets(d->device, 1, &w, 0, NULL);
    return AEVK_OK;
}

int aevk_target_set_push(AevkTarget* t, const void* data, size_t len) {
    aevk_clear_error();
    if (!t) return aevk_fail(AEVK_ERR_ARG, "target is null");
    if (len > AEVK_MAX_PUSH) {
        return aevk_fail(AEVK_ERR_ARG, "push constants are at most %d bytes, got %zu",
                         AEVK_MAX_PUSH, len);
    }
    if (len > 0 && !data) return aevk_fail(AEVK_ERR_ARG, "push data is null");
    if (len > 0) memcpy(t->push_data, data, len);
    t->push_size = (uint32_t)len;
    return AEVK_OK;
}

/* Defined with the other Aether-facing entry points below; the C API's
 * set_vertices is a thin wrapper over it so the grow-and-map path exists
 * once. */
int aevk_ae_verts_reserve(void* tp, int count);

/* ------------------------------------------------------------------------ */
/* Geometry, draw, readback                                                  */
/* ------------------------------------------------------------------------ */

int aevk_target_set_vertices(AevkTarget* t, const float* data, int count) {
    if (!data) return aevk_fail(AEVK_ERR_ARG, "data is null");
    int rc = aevk_ae_verts_reserve((void*)t, count);
    if (rc != AEVK_OK) return rc;
    memcpy(t->vbuf_ptr, data, (size_t)count * 5u * sizeof(float));
    return AEVK_OK;
}

static int aevk_record(AevkTarget* t, AevkPipeline* p,
                       float r, float g, float b, float a) {
    AevkDevice* d = t->dev;
    VkResult vr = d->da.vkResetCommandBuffer(t->cmd, 0);
    if (vr != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkResetCommandBuffer failed (%d)", (int)vr);

    VkCommandBufferBeginInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vr = d->da.vkBeginCommandBuffer(t->cmd, &bi);
    if (vr != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkBeginCommandBuffer failed (%d)", (int)vr);

    VkClearValue clear;
    clear.color.float32[0] = r;
    clear.color.float32[1] = g;
    clear.color.float32[2] = b;
    clear.color.float32[3] = a;

    VkRenderPassBeginInfo rp = {0};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = t->pass;
    rp.framebuffer = t->fb;
    rp.renderArea.extent.width = (uint32_t)t->width;
    rp.renderArea.extent.height = (uint32_t)t->height;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;

    d->da.vkCmdBeginRenderPass(t->cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    if (p && t->vertex_count > 0) {
        VkViewport vp = {0};
        vp.width = (float)t->width;
        vp.height = (float)t->height;
        vp.maxDepth = 1.0f;
        VkRect2D sc = {{0, 0}, {(uint32_t)t->width, (uint32_t)t->height}};
        d->da.vkCmdSetViewport(t->cmd, 0, 1, &vp);
        d->da.vkCmdSetScissor(t->cmd, 0, 1, &sc);
        d->da.vkCmdBindPipeline(t->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
        if (p->set) {
            d->da.vkCmdBindDescriptorSets(t->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          p->layout, 0, 1, &p->set, 0, NULL);
        }
        /* Push whatever the pipeline declared room for, so a caller who set
         * fewer bytes than the range still gets a defined block. */
        if (p->push_bytes > 0) {
            unsigned char block[AEVK_MAX_PUSH];
            memset(block, 0, sizeof(block));
            uint32_t n = t->push_size < p->push_bytes ? t->push_size : p->push_bytes;
            if (n) memcpy(block, t->push_data, n);
            d->da.vkCmdPushConstants(t->cmd, p->layout,
                                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                     0, p->push_bytes, block);
        }
        VkDeviceSize off = 0;
        d->da.vkCmdBindVertexBuffers(t->cmd, 0, 1, &t->vbuf, &off);
        if (t->index_count > 0) {
            d->da.vkCmdBindIndexBuffer(t->cmd, t->ibuf, 0, VK_INDEX_TYPE_UINT32);
            d->da.vkCmdDrawIndexed(t->cmd, (uint32_t)t->index_count, 1, 0, 0, 0);
        } else {
            d->da.vkCmdDraw(t->cmd, (uint32_t)t->vertex_count, 1, 0, 0);
        }
    }
    d->da.vkCmdEndRenderPass(t->cmd);

    VkBufferImageCopy copy = {0};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = (uint32_t)t->width;
    copy.imageExtent.height = (uint32_t)t->height;
    copy.imageExtent.depth = 1;
    d->da.vkCmdCopyImageToBuffer(t->cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 t->readback, 1, &copy);

    vr = d->da.vkEndCommandBuffer(t->cmd);
    if (vr != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkEndCommandBuffer failed (%d)", (int)vr);

    t->recorded = 1;
    t->rec_pipe = p;
    t->rec_vertices = t->vertex_count;
    t->rec_clear[0] = r; t->rec_clear[1] = g; t->rec_clear[2] = b; t->rec_clear[3] = a;
    t->rec_indices = t->index_count;
    t->rec_push_size = t->push_size;
    if (t->push_size) memcpy(t->rec_push, t->push_data, t->push_size);
    return AEVK_OK;
}

int aevk_draw(AevkTarget* t, AevkPipeline* p, float r, float g, float b, float a) {
    aevk_clear_error();
    if (!t) return aevk_fail(AEVK_ERR_ARG, "target is null");
    if (p && p->dev != t->dev) return aevk_fail(AEVK_ERR_ARG, "pipeline belongs to another device");
    if (p && t->vertex_count > 0 && !t->vbuf) {
        return aevk_fail(AEVK_ERR_ARG, "vertices were never uploaded");
    }

    AevkDevice* d = t->dev;
    int stale = !t->recorded || t->rec_pipe != p || t->rec_vertices != t->vertex_count ||
                t->rec_clear[0] != r || t->rec_clear[1] != g ||
                t->rec_clear[2] != b || t->rec_clear[3] != a ||
                t->rec_indices != t->index_count ||
                t->rec_push_size != t->push_size ||
                (t->push_size && memcmp(t->rec_push, t->push_data, t->push_size) != 0);
    if (stale) {
        int rc = aevk_record(t, p, r, g, b, a);
        if (rc != AEVK_OK) { t->recorded = 0; return rc; }
    }

    VkResult vr = d->da.vkResetFences(d->device, 1, &t->fence);
    if (vr != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkResetFences failed (%d)", (int)vr);

    VkSubmitInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &t->cmd;
    vr = d->da.vkQueueSubmit(d->queue, 1, &si, t->fence);
    if (vr != VK_SUCCESS) {
        return aevk_fail(vr == VK_ERROR_DEVICE_LOST ? AEVK_ERR_DEVICE_LOST : AEVK_ERR_OOM,
                         "vkQueueSubmit failed (%d)", (int)vr);
    }

    /* Five seconds is not a frame budget, it is a hang detector: a correct
     * driver finishes this work in microseconds, so anything near it means the
     * device is wedged and returning beats blocking forever. */
    vr = d->da.vkWaitForFences(d->device, 1, &t->fence, VK_TRUE, 5000000000ull);
    if (vr == VK_TIMEOUT) return aevk_fail(AEVK_ERR_DEVICE_LOST, "GPU did not finish within 5s");
    if (vr != VK_SUCCESS) {
        return aevk_fail(vr == VK_ERROR_DEVICE_LOST ? AEVK_ERR_DEVICE_LOST : AEVK_ERR_OOM,
                         "vkWaitForFences failed (%d)", (int)vr);
    }
    return AEVK_OK;
}

int aevk_read_rgba(AevkTarget* t, void* out, size_t out_len) {
    aevk_clear_error();
    if (!t || !out) return aevk_fail(AEVK_ERR_ARG, "target or destination is null");
    if (out_len < (size_t)t->readback_size) {
        return aevk_fail(AEVK_ERR_ARG, "destination holds %zu bytes, the image needs %zu",
                         out_len, (size_t)t->readback_size);
    }
    if (!t->readback_ptr) return aevk_fail(AEVK_ERR_ARG, "target has no readback mapping");
    memcpy(out, t->readback_ptr, (size_t)t->readback_size);
    return AEVK_OK;
}

/* ------------------------------------------------------------------------ */
/* Aether-facing entry points                                                */
/* ------------------------------------------------------------------------ */
/* Signatures match exactly what aetherc emits for an `extern`: `ptr` is
 * void*, `string` is const char*, `int` is int and `float` is double. A
 * mismatch here is a conflicting-declaration error at build time, so these
 * cannot silently drift from module.ae. */

int   aevk_ae_available(void)            { return aevk_available(); }
const char* aevk_ae_last_error(void)     { return aevk_last_error(); }
const char* aevk_ae_device_name(void)    { return aevk_device_name(); }

void* aevk_ae_device_create(void)        { return (void*)aevk_device_create(); }
void  aevk_ae_device_destroy(void* d)    { aevk_device_destroy((AevkDevice*)d); }

void* aevk_ae_target_create(void* d, int w, int h) {
    return (void*)aevk_target_create((AevkDevice*)d, w, h);
}
void  aevk_ae_target_destroy(void* t)    { aevk_target_destroy((AevkTarget*)t); }
int   aevk_ae_target_width(void* t)      { return aevk_target_width((AevkTarget*)t); }
int   aevk_ae_target_height(void* t)     { return aevk_target_height((AevkTarget*)t); }
int   aevk_ae_rgba_size(void* t)         { return (int)aevk_rgba_size((AevkTarget*)t); }

void* aevk_ae_pipeline_create(void* d, void* t,
                              const char* vspv, int vlen,
                              const char* fspv, int flen) {
    if (vlen < 0 || flen < 0) {
        aevk_fail(AEVK_ERR_SHADER, "negative SPIR-V length");
        return NULL;
    }
    return (void*)aevk_pipeline_create((AevkDevice*)d, (AevkTarget*)t,
                                       vspv, (size_t)vlen, fspv, (size_t)flen);
}
void  aevk_ae_pipeline_destroy(void* p)  { aevk_pipeline_destroy((AevkPipeline*)p); }

void* aevk_ae_pipeline_create_ex(void* d, void* t,
                                 const char* vspv, int vlen,
                                 const char* fspv, int flen,
                                 void* layout, int push_bytes, void* bindings) {
    if (vlen < 0 || flen < 0) {
        aevk_fail(AEVK_ERR_SHADER, "negative SPIR-V length");
        return NULL;
    }
    return (void*)aevk_pipeline_create_ex((AevkDevice*)d, (AevkTarget*)t,
                                          vspv, (size_t)vlen, fspv, (size_t)flen,
                                          (const AevkLayout*)layout, push_bytes,
                                          (const AevkBindings*)bindings);
}

void* aevk_ae_layout_create(void) { return (void*)aevk_layout_create(); }
void  aevk_ae_layout_destroy(void* l) { aevk_layout_destroy((AevkLayout*)l); }
int   aevk_ae_layout_binding(void* l, int binding, int stride, int per_instance) {
    return aevk_layout_binding((AevkLayout*)l, binding, stride, per_instance);
}
int   aevk_ae_layout_attr(void* l, int location, int binding, int format, int offset) {
    return aevk_layout_attr((AevkLayout*)l, location, binding, format, offset);
}

void* aevk_ae_bindings_create(void) { return (void*)aevk_bindings_create(); }
void  aevk_ae_bindings_destroy(void* b) { aevk_bindings_destroy((AevkBindings*)b); }
int   aevk_ae_bindings_uniform(void* b, int binding) {
    return aevk_bindings_uniform((AevkBindings*)b, binding);
}
int   aevk_ae_bindings_texture(void* b, int binding) {
    return aevk_bindings_texture((AevkBindings*)b, binding);
}

void* aevk_ae_texture_create(void* d, int w, int h) {
    return (void*)aevk_texture_create((AevkDevice*)d, w, h);
}
void  aevk_ae_texture_destroy(void* tex) { aevk_texture_destroy((AevkTexture*)tex); }
int   aevk_ae_texture_upload(void* tex, const void* rgba, int len) {
    if (len < 0) return aevk_fail(AEVK_ERR_ARG, "negative pixel length");
    return aevk_texture_upload((AevkTexture*)tex, rgba, (size_t)len);
}

int aevk_ae_set_texture(void* p, int binding, void* tex) {
    return aevk_pipeline_set_texture((AevkPipeline*)p, binding, (AevkTexture*)tex);
}

int aevk_ae_set_uniform(void* p, int binding, const void* data, int len) {
    if (len < 0) return aevk_fail(AEVK_ERR_ARG, "negative uniform length");
    return aevk_pipeline_set_uniform((AevkPipeline*)p, binding, data, (size_t)len);
}

/* Float staging for push constants and uniforms. Callers describe matrices
 * and vectors as floats; without these they would have to pack IEEE bytes by
 * hand on the Aether side to say "identity matrix". */
int aevk_ae_push_floats(void* tp, int count) {
    AevkTarget* t = (AevkTarget*)tp;
    aevk_clear_error();
    if (!t) return aevk_fail(AEVK_ERR_ARG, "target is null");
    if (count < 0 || (size_t)count * sizeof(float) > AEVK_MAX_PUSH) {
        return aevk_fail(AEVK_ERR_ARG, "push block of %d floats exceeds %d bytes",
                         count, AEVK_MAX_PUSH);
    }
    memset(t->push_data, 0, sizeof(t->push_data));
    t->push_size = (uint32_t)((size_t)count * sizeof(float));
    return AEVK_OK;
}

int aevk_ae_push_float(void* tp, int index, double value) {
    AevkTarget* t = (AevkTarget*)tp;
    aevk_clear_error();
    if (!t) return aevk_fail(AEVK_ERR_ARG, "target is null");
    int n = (int)(t->push_size / sizeof(float));
    if (index < 0 || index >= n) {
        return aevk_fail(AEVK_ERR_ARG, "push float %d is outside 0..%d", index, n - 1);
    }
    float f = (float)value;
    memcpy(t->push_data + (size_t)index * sizeof(float), &f, sizeof(f));
    return AEVK_OK;
}

int aevk_ae_uniform_floats(void* pp, int binding, int count) {
    AevkPipeline* p = (AevkPipeline*)pp;
    aevk_clear_error();
    if (!p) return aevk_fail(AEVK_ERR_ARG, "pipeline is null");
    if (count <= 0) return aevk_fail(AEVK_ERR_ARG, "uniform float count must be positive");
    size_t bytes = (size_t)count * sizeof(float);
    float stack[64];
    float* zero = stack;
    if (count > (int)(sizeof(stack) / sizeof(stack[0]))) {
        zero = (float*)calloc((size_t)count, sizeof(float));
        if (!zero) return aevk_fail(AEVK_ERR_OOM, "out of memory");
    } else {
        memset(stack, 0, bytes);
    }
    int rc = aevk_pipeline_set_uniform(p, binding, zero, bytes);
    if (zero != stack) free(zero);
    return rc;
}

int aevk_ae_uniform_float(void* pp, int binding, int index, double value) {
    AevkPipeline* p = (AevkPipeline*)pp;
    aevk_clear_error();
    if (!p) return aevk_fail(AEVK_ERR_ARG, "pipeline is null");
    if (binding < 0 || binding >= AEVK_MAX_DESC) {
        return aevk_fail(AEVK_ERR_ARG, "binding must be 0..%d", AEVK_MAX_DESC - 1);
    }
    if (!p->ub[binding].ptr) {
        return aevk_fail(AEVK_ERR_ARG, "call uniform_floats for binding %d first", binding);
    }
    int n = (int)(p->ub[binding].size / sizeof(float));
    if (index < 0 || index >= n) {
        return aevk_fail(AEVK_ERR_ARG, "uniform float %d is outside 0..%d", index, n - 1);
    }
    float f = (float)value;
    memcpy((char*)p->ub[binding].ptr + (size_t)index * sizeof(float), &f, sizeof(f));
    return AEVK_OK;
}

int aevk_ae_set_push(void* t, const void* data, int len) {
    if (len < 0) return aevk_fail(AEVK_ERR_ARG, "negative push length");
    return aevk_target_set_push((AevkTarget*)t, data, (size_t)len);
}

/* Reserve then write: aevk_ae_verts_set stores straight into the mapped
 * vertex buffer, so geometry crosses from Aether to GPU memory without an
 * intermediate host array. */
/* `fpv` is floats per vertex: 5 for the built-in position+colour layout,
 * whatever the caller's own layout says otherwise. Capacity is tracked in
 * floats so switching layouts on one target cannot under-allocate. */
static int aevk_verts_reserve_impl(AevkTarget* t, int count, int fpv) {
    aevk_clear_error();
    if (!t) return aevk_fail(AEVK_ERR_ARG, "target is null");
    if (count <= 0) return aevk_fail(AEVK_ERR_ARG, "vertex count must be positive, got %d", count);
    if (fpv <= 0) return aevk_fail(AEVK_ERR_ARG, "floats per vertex must be positive, got %d", fpv);
    if ((size_t)count > (size_t)(SIZE_MAX / ((size_t)fpv * sizeof(float)))) {
        return aevk_fail(AEVK_ERR_ARG, "vertex count %d is too large", count);
    }

    AevkDevice* d = t->dev;
    int floats_now = count * fpv;
    int floats_cap = t->vbuf_capacity * (t->vertex_floats ? t->vertex_floats : 5);
    if (floats_now > floats_cap) {
        VkDeviceSize need = (VkDeviceSize)count * (VkDeviceSize)fpv * sizeof(float);
        d->da.vkDeviceWaitIdle(d->device);
        if (t->vbuf_ptr) { d->da.vkUnmapMemory(d->device, t->vbuf_mem); t->vbuf_ptr = NULL; }
        if (t->vbuf)     { d->da.vkDestroyBuffer(d->device, t->vbuf, NULL); t->vbuf = VK_NULL_HANDLE; }
        if (t->vbuf_mem) { d->da.vkFreeMemory(d->device, t->vbuf_mem, NULL); t->vbuf_mem = VK_NULL_HANDLE; }
        t->vbuf_capacity = 0;

        int rc = aevk_make_buffer(d, need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  &t->vbuf, &t->vbuf_mem);
        if (rc != AEVK_OK) return rc;
        VkResult r = d->da.vkMapMemory(d->device, t->vbuf_mem, 0, need, 0, &t->vbuf_ptr);
        if (r != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkMapMemory failed (%d)", (int)r);
        t->vbuf_capacity = count;
    }
    if (t->vertex_count != count || t->vertex_floats != fpv) t->recorded = 0;
    t->vertex_count = count;
    t->vertex_floats = fpv;
    return AEVK_OK;
}

int aevk_ae_verts_reserve(void* tp, int count) {
    return aevk_verts_reserve_impl((AevkTarget*)tp, count, 5);
}

int aevk_ae_verts_reserve_n(void* tp, int count, int floats_per_vertex) {
    return aevk_verts_reserve_impl((AevkTarget*)tp, count, floats_per_vertex);
}

/* Writes one float at a flat index into the vertex block, so a caller with
 * its own layout does not need a per-shape setter. */
int aevk_ae_verts_set_float(void* tp, int float_index, double value) {
    AevkTarget* t = (AevkTarget*)tp;
    aevk_clear_error();
    if (!t || !t->vbuf_ptr) return aevk_fail(AEVK_ERR_ARG, "reserve vertices first");
    int total = t->vertex_count * t->vertex_floats;
    if (float_index < 0 || float_index >= total) {
        return aevk_fail(AEVK_ERR_ARG, "float index %d is outside 0..%d",
                         float_index, total - 1);
    }
    ((float*)t->vbuf_ptr)[float_index] = (float)value;
    t->recorded = 0;
    return AEVK_OK;
}

/* Index buffer. 32-bit indices only: the memory saved by 16-bit ones is not
 * worth a second code path at this size, and a caller who needs it can say
 * so when there is a reason. */
int aevk_ae_indices_reserve(void* tp, int count) {
    AevkTarget* t = (AevkTarget*)tp;
    aevk_clear_error();
    if (!t) return aevk_fail(AEVK_ERR_ARG, "target is null");
    if (count < 0) return aevk_fail(AEVK_ERR_ARG, "index count must not be negative");
    if ((size_t)count > (size_t)(SIZE_MAX / sizeof(uint32_t))) {
        return aevk_fail(AEVK_ERR_ARG, "index count %d is too large", count);
    }

    AevkDevice* d = t->dev;
    if (count > t->ibuf_capacity) {
        VkDeviceSize need = (VkDeviceSize)count * sizeof(uint32_t);
        d->da.vkDeviceWaitIdle(d->device);
        if (t->ibuf_ptr) { d->da.vkUnmapMemory(d->device, t->ibuf_mem); t->ibuf_ptr = NULL; }
        if (t->ibuf)     { d->da.vkDestroyBuffer(d->device, t->ibuf, NULL); t->ibuf = VK_NULL_HANDLE; }
        if (t->ibuf_mem) { d->da.vkFreeMemory(d->device, t->ibuf_mem, NULL); t->ibuf_mem = VK_NULL_HANDLE; }
        t->ibuf_capacity = 0;

        int rc = aevk_make_buffer(d, need, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  &t->ibuf, &t->ibuf_mem);
        if (rc != AEVK_OK) return rc;
        VkResult r = d->da.vkMapMemory(d->device, t->ibuf_mem, 0, need, 0, &t->ibuf_ptr);
        if (r != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkMapMemory failed (%d)", (int)r);
        t->ibuf_capacity = count;
    }
    if (t->index_count != count) t->recorded = 0;
    t->index_count = count;
    return AEVK_OK;
}

int aevk_ae_indices_set(void* tp, int index, int value) {
    AevkTarget* t = (AevkTarget*)tp;
    aevk_clear_error();
    if (!t || !t->ibuf_ptr) return aevk_fail(AEVK_ERR_ARG, "reserve indices first");
    if (index < 0 || index >= t->index_count) {
        return aevk_fail(AEVK_ERR_ARG, "index %d is outside 0..%d", index, t->index_count - 1);
    }
    if (value < 0) return aevk_fail(AEVK_ERR_ARG, "vertex index must not be negative");
    if (value >= t->vertex_count) {
        return aevk_fail(AEVK_ERR_ARG, "index %d points past the %d uploaded vertices",
                         value, t->vertex_count);
    }
    ((uint32_t*)t->ibuf_ptr)[index] = (uint32_t)value;
    t->recorded = 0;
    return AEVK_OK;
}

int aevk_ae_verts_set(void* tp, int index,
                      double x, double y, double r, double g, double b) {
    AevkTarget* t = (AevkTarget*)tp;
    if (!t || !t->vbuf_ptr) return aevk_fail(AEVK_ERR_ARG, "reserve vertices first");
    if (index < 0 || index >= t->vertex_count) {
        return aevk_fail(AEVK_ERR_ARG, "vertex %d is outside 0..%d", index, t->vertex_count - 1);
    }
    float* v = (float*)t->vbuf_ptr + (size_t)index * 5u;
    v[0] = (float)x; v[1] = (float)y;
    v[2] = (float)r; v[3] = (float)g; v[4] = (float)b;
    return AEVK_OK;
}

int aevk_ae_draw(void* t, void* p, double r, double g, double b, double a) {
    return aevk_draw((AevkTarget*)t, (AevkPipeline*)p,
                     (float)r, (float)g, (float)b, (float)a);
}

int aevk_ae_copy_rgba(void* t, void* dest, int dest_len) {
    if (dest_len < 0) return aevk_fail(AEVK_ERR_ARG, "negative destination length");
    return aevk_read_rgba((AevkTarget*)t, dest, (size_t)dest_len);
}

/* Packed 0xRRGGBBAA for one pixel, or -1 when the coordinates are outside the
 * image. Reads the mapped buffer directly, so a test can sample without
 * copying the whole frame. */
int aevk_ae_pixel(void* tp, int x, int y) {
    AevkTarget* t = (AevkTarget*)tp;
    aevk_clear_error();
    if (!t || !t->readback_ptr) { aevk_fail(AEVK_ERR_ARG, "target has no readback"); return -1; }
    if (x < 0 || y < 0 || x >= t->width || y >= t->height) {
        aevk_fail(AEVK_ERR_ARG, "pixel %d,%d is outside %dx%d", x, y, t->width, t->height);
        return -1;
    }
    const unsigned char* px =
        (const unsigned char*)t->readback_ptr + ((size_t)y * (size_t)t->width + (size_t)x) * 4u;
    return (int)(((unsigned)px[0] << 24) | ((unsigned)px[1] << 16) |
                 ((unsigned)px[2] << 8)  |  (unsigned)px[3]);
}

/* Binary PPM (P6). Chosen over PNG because it needs no compressor, so the
 * example and the CI leg carry no extra dependency and the bytes on disk are
 * trivially checkable. */
int aevk_ae_save_ppm(void* tp, const char* path) {
    AevkTarget* t = (AevkTarget*)tp;
    aevk_clear_error();
    if (!t || !path) return aevk_fail(AEVK_ERR_ARG, "target or path is null");
    if (!t->readback_ptr) return aevk_fail(AEVK_ERR_ARG, "target has no readback");

    FILE* f = fopen(path, "wb");
    if (!f) return aevk_fail(AEVK_ERR_ARG, "cannot open %s for writing", path);
    if (fprintf(f, "P6\n%d %d\n255\n", t->width, t->height) < 0) {
        fclose(f);
        return aevk_fail(AEVK_ERR_ARG, "cannot write the PPM header to %s", path);
    }

    const unsigned char* src = (const unsigned char*)t->readback_ptr;
    size_t pixels = (size_t)t->width * (size_t)t->height;
    for (size_t i = 0; i < pixels; i++) {
        if (fwrite(src + i * 4u, 1, 3, f) != 3) {
            fclose(f);
            return aevk_fail(AEVK_ERR_ARG, "short write to %s", path);
        }
    }
    if (fclose(f) != 0) return aevk_fail(AEVK_ERR_ARG, "cannot flush %s", path);
    return AEVK_OK;
}
