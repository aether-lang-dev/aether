/* contrib/vulkan: offscreen GPU rendering for Aether (#1495, phase 1).
 * See aether_vulkan.h for the API and the runtime-loading rationale.
 */

#include "aether_vulkan.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#endif

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

/* The window-system surface extensions (#1505). Each platform header is
 * included on its own rather than through VK_USE_PLATFORM_*, so a missing
 * window-system header costs that one kind of window and not the build:
 * Wayland's structs name only opaque pointers, so forward declarations do,
 * while Xlib's name X11 types and need the X11 headers present. */
#if defined(_WIN32)
#  include <vulkan/vulkan_win32.h>
#  define AEVK_HAVE_WIN32_SURFACE 1
#elif defined(__APPLE__)
#  include <vulkan/vulkan_metal.h>
#  define AEVK_HAVE_METAL_SURFACE 1
#else
struct wl_display;
struct wl_surface;
#  include <vulkan/vulkan_wayland.h>
#  define AEVK_HAVE_WAYLAND_SURFACE 1
#  if defined(__has_include)
#    if __has_include(<X11/Xlib.h>)
#      include <X11/Xlib.h>
#      include <vulkan/vulkan_xlib.h>
#      define AEVK_HAVE_XLIB_SURFACE 1
#    endif
#  endif
#endif

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
#  define AEVK_DLOPEN(p)      ((void*)LoadLibraryA(p))
#  define AEVK_DLSYM(h, s)    ((void*)GetProcAddress((HMODULE)(h), (s)))
#  define AEVK_DLCLOSE(h)     FreeLibrary((HMODULE)(h))
#else
#  include <dlfcn.h>
#  define AEVK_DLOPEN(p)      dlopen((p), RTLD_NOW | RTLD_LOCAL)
#  define AEVK_DLSYM(h, s)    dlsym((h), (s))
#  define AEVK_DLCLOSE(h)     dlclose(h)
#endif

/* A mutex, spelled locally for the same reason the dlopen shim above is:
 * this module builds against the Vulkan headers and nothing else, so it can be
 * copied out of the tree. Reaching into runtime/utils/aether_thread.h would
 * make that false for one lock.
 *
 * Vulkan requires the CALLER to synchronise a VkQueue and a VkCommandPool.
 * Aether is an actor language, so two actors sharing one device is the
 * expected shape rather than an exotic one, and "undefined behaviour if you
 * do the natural thing" is not a contract worth shipping (#1510). One lock
 * per device covers both objects; every public entry point that touches
 * either takes it, and the internal helpers never do, so the paths that
 * compose (texture upload runs a command buffer) cannot deadlock. */
#ifdef _WIN32
   typedef SRWLOCK AevkMutex;
#  define AEVK_MUTEX_INIT(m)    InitializeSRWLock(m)
#  define AEVK_MUTEX_LOCK(m)    AcquireSRWLockExclusive(m)
#  define AEVK_MUTEX_UNLOCK(m)  ReleaseSRWLockExclusive(m)
#  define AEVK_MUTEX_DESTROY(m) ((void)(m))
#  define AEVK_MUTEX_STATIC      SRWLOCK_INIT
#else
#  include <pthread.h>
   typedef pthread_mutex_t AevkMutex;
#  define AEVK_MUTEX_INIT(m)    pthread_mutex_init((m), NULL)
#  define AEVK_MUTEX_LOCK(m)    pthread_mutex_lock(m)
#  define AEVK_MUTEX_UNLOCK(m)  pthread_mutex_unlock(m)
#  define AEVK_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#  define AEVK_MUTEX_STATIC      PTHREAD_MUTEX_INITIALIZER
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

/* The entry points this file loads, as X-macro lists generated from the
 * Vulkan registry by tools/vkgen.ae (#1506): one list per feature or
 * extension and loading level, from tools/dispatch_commands.txt. GLOBAL and
 * INSTANCE ones come from vkGetInstanceProcAddr; DEVICE ones from
 * vkGetDeviceProcAddr, which returns the driver's own function and skips the
 * loader's dispatch trampoline. The window-system lists (#1505) are loaded
 * only when the loader and device advertise those extensions, and are NULL
 * otherwise, so a driver without a window system still renders offscreen. */
#include "aether_vulkan_dispatch.h"

#define AEVK_DECL(name) PFN_##name name;

typedef struct {
    AEVK_GLOBAL_FNS(AEVK_DECL)
    AEVK_INSTANCE_FNS(AEVK_DECL)
    AEVK_KHR_SURFACE_FNS(AEVK_DECL)
#if defined(AEVK_HAVE_WIN32_SURFACE)
    AEVK_KHR_WIN32_SURFACE_FNS(AEVK_DECL)
#endif
#if defined(AEVK_HAVE_METAL_SURFACE)
    AEVK_EXT_METAL_SURFACE_FNS(AEVK_DECL)
#endif
#if defined(AEVK_HAVE_WAYLAND_SURFACE)
    AEVK_KHR_WAYLAND_SURFACE_FNS(AEVK_DECL)
#endif
#if defined(AEVK_HAVE_XLIB_SURFACE)
    AEVK_KHR_XLIB_SURFACE_FNS(AEVK_DECL)
#endif
} AevkInstanceApi;

typedef struct {
    AEVK_DEVICE_FNS(AEVK_DECL)
    AEVK_KHR_SWAPCHAIN_FNS(AEVK_DECL)
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

/* Under a lock, every time: callers on several threads (two actors, or the
 * first calls of contrib.vulkan.vk's commands) must neither open the loader
 * twice nor see g_lib set while g_gipa is not yet. It runs when a device is
 * made and when a command is first resolved, never per frame. */
static int aevk_load_library(void) {
    static AevkMutex load_lock = AEVK_MUTEX_STATIC;
    AEVK_MUTEX_LOCK(&load_lock);
    int rc = AEVK_OK;
    if (!g_lib) {
        rc = AEVK_ERR_NO_LOADER;
        for (int i = 0; k_loader_names[i]; i++) {
            void* h = AEVK_DLOPEN(k_loader_names[i]);
            if (!h) continue;
            PFN_vkGetInstanceProcAddr gipa =
                (PFN_vkGetInstanceProcAddr)AEVK_DLSYM(h, "vkGetInstanceProcAddr");
            if (!gipa) { AEVK_DLCLOSE(h); continue; }
            g_gipa = gipa;
            g_lib = h;
            rc = AEVK_OK;
            break;
        }
        if (rc != AEVK_OK) {
            aevk_fail(AEVK_ERR_NO_LOADER, "no Vulkan loader found (tried %s and friends)",
                      k_loader_names[0]);
        }
    }
    AEVK_MUTEX_UNLOCK(&load_lock);
    return rc;
}

/* ------------------------------------------------------------------------ */
/* Objects                                                                   */
/* ------------------------------------------------------------------------ */

struct AevkDevice {
    /* Held across queue submission and any command-pool access. See the
     * AevkMutex note above for why this exists and what it covers. */
    AevkMutex        lock;
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
    /* The largest dispatch the device takes, per dimension (#1515). */
    uint32_t         max_groups[3];
    /* Which sample counts the device can actually use for framebuffer colour
     * and depth. Asking for 4x on hardware that offers 2x is a caller error
     * worth naming, not something to silently round down. */
    VkSampleCountFlags sample_counts;
    /* Which window kinds this instance can make a surface for (a bit per
     * AEVK_WINDOW_* value), and whether the device can present at all. Both
     * are what the loader and driver advertised, not what the platform could
     * do in principle. */
    unsigned         surface_kinds;
    int              can_present;
    char             name[256];
};

/* Caller-described vertex input. Fixed capacity rather than a growable
 * array: a pipeline with more than this many streams or attributes is past
 * the point where a description struct is the right interface. */
#define AEVK_MAX_BINDINGS 8
#define AEVK_MAX_ATTRS    16
#define AEVK_MAX_DESC     8
#define AEVK_MAX_PUSH     128   /* the guaranteed minimum every device offers */

/* One draw inside a frame. `first`/`count` address indices when the target
 * has an index buffer and vertices otherwise, so a batch slices whatever
 * geometry is already uploaded rather than needing its own copy. */
typedef struct {
    AevkMaterial* mat;
    int           first;
    int           count;
} AevkDrawItem;

typedef struct {
    VkCommandBuffer cmd;
    VkFence         fence;
    VkBuffer        readback;
    VkDeviceMemory  readback_mem;
    void*           readback_ptr;   /* mapped for the slot's lifetime */
    int             submitted;      /* work handed to the queue, not yet waited on */

    /* What this slot's command buffer currently holds, so a repeat draw skips
     * re-recording. Per slot, not per target: with several in flight they hold
     * different frames. Anything the recording baked in has to take part in
     * the reuse test, or a changed transform silently redraws the old frame,
     * so push bytes are compared by value rather than by size alone. */
    int             recorded;
    AevkPipeline*   rec_pipe;
    AevkMaterial*   rec_mat;
    int             rec_vertices;
    int             rec_indices;
    float           rec_clear[4];
    uint32_t        rec_push_size;
    unsigned char   rec_push[AEVK_MAX_PUSH];
    /* The batch is compared by version rather than by value: every mutation
     * bumps it, so a slot holding an older list re-records. Material CONTENTS
     * are deliberately not part of this; a descriptor set and a mapped uniform
     * buffer are read when the GPU executes, not when the command is
     * recorded. */
    unsigned        rec_batch_version;
    int             rec_batch_count;
} AevkFrame;

struct AevkTarget {
    AevkDevice*    dev;
    int            width, height;
    /* The colour format rendered, read back and presented (#1514), and its
     * size in bytes a pixel, which the readback buffers are sized by. */
    VkFormat       color_format;
    int            bytes_per_pixel;
    /* 0 until a frame has been submitted, so the colour image has contents
     * and sits in TRANSFER_SRC_OPTIMAL: presenting reads it from there. */
    int            rendered;
    /* Whether each frame is copied into the readback buffer. On by default;
     * a target that is only presented turns it off and stops paying a
     * width*height*4 copy per frame (target_set_readback). */
    int            readback_on;
    VkImage        image;
    VkDeviceMemory image_mem;
    VkImageView    view;
    VkRenderPass   pass;
    VkFramebuffer  fb;

    VkDeviceSize   readback_size;

    /* Multisampling and depth, both optional (#1512). With samples > 1 the
     * colour attachment is multisampled and resolves into `image`, which stays
     * single-sample so readback is unchanged. */
    int            samples;
    VkImage        msaa_image;
    VkDeviceMemory msaa_mem;
    VkImageView    msaa_view;

    int            has_depth;
    VkFormat       depth_format;
    uint32_t       clear_count;        /* attachments, so record() sizes pClearValues */
    uint32_t       depth_clear_index;  /* where the depth clear goes in that array */
    VkImage        depth_image;
    VkDeviceMemory depth_mem;
    VkImageView    depth_view;

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
    int            index_bits;      /* 16 or 32 */

    AevkDrawItem*  batch;
    int            batch_count;
    int            batch_cap;
    unsigned       batch_version;

    unsigned char  push_data[AEVK_MAX_PUSH];
    uint32_t       push_size;

    /* Frames in flight (#1513). One slot is the synchronous shape phase 1
     * shipped and stays the default; more than one lets the CPU record and
     * submit while the GPU is still working on an earlier frame.
     *
     * Each slot owns its command buffer, its fence and its OWN readback
     * buffer: sharing one readback across frames in flight would have frame
     * N+1 overwrite pixels frame N had not been read yet. */
    AevkFrame*      frames;
    int             frame_count;
    int             next_frame;     /* round-robin cursor */
    int             last_done;      /* most recent slot waited on */
    int             last_submitted; /* newest frame, which is what readers want */
    uint64_t        timeout_ns;
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
    uint32_t       mip_levels;
    VkImage        image;
    VkDeviceMemory mem;
    VkImageView    view;
    VkSampler      sampler;
    int            uploaded;   /* 0 until the first upload lays it out */
};

/* One descriptor set plus the uniform buffers written into it: a material.
 * Several can exist per pipeline, so one pipeline draws several objects with
 * different textures and constants in a frame instead of needing a pipeline
 * per material, which would duplicate the shader modules for nothing. */
/* A storage buffer (#1515): host-visible and mapped for its lifetime, so the
 * CPU fills it and reads results back with a memcpy. The same buffer serves
 * a compute pass and a graphics pipeline, as a storage or a uniform binding,
 * which is how compute output feeds a draw without a copy. */
struct AevkBuffer {
    AevkDevice*    dev;
    VkBuffer       buf;
    VkDeviceMemory mem;
    void*          ptr;
    VkDeviceSize   size;
};

struct AevkMaterial {
    AevkPipeline*   pipe;
    VkDescriptorSet set;
    /* Descriptors actually written into `set`. A set straight out of the pool
     * holds nothing, and binding one is what crashed lavapipe 22.3 from inside
     * the driver: a software rasteriser walks the set as it is bound, so the
     * unwritten image/buffer descriptors are dereferenced there and then.
     * Nothing is bound until it has contents. */
    int             writes;
    struct {
        VkBuffer       buf;
        VkDeviceMemory mem;
        void*          ptr;
        VkDeviceSize   size;
    } ub[AEVK_MAX_DESC];
    /* A caller's buffer bound in place of the material's own uniform (or at
     * a storage binding), so a uniform written later knows to re-point the
     * descriptor at the material's buffer. */
    AevkBuffer*     ext[AEVK_MAX_DESC];
};

#define AEVK_SETS_PER_POOL 16
#define AEVK_MAX_POOLS     64

struct AevkPipeline {
    AevkDevice*      dev;
    VkShaderModule   vert, frag;
    VkPipelineLayout layout;
    VkPipeline       pipeline;
    uint32_t         push_bytes;
    /* 0 for a pipeline made with an empty layout, whose vertex shader pulls
     * its data itself: no vertex buffer is bound for it. */
    int              vertex_input;

    /* Descriptor pools, grown a block at a time: a fixed maxSets would put a
     * ceiling on how many materials a scene can have, and sizing one pool for
     * the worst case would waste memory for the common one. */
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool      pools[AEVK_MAX_POOLS];
    int                   pool_count;
    int                   sets_in_pool;   /* used in the newest pool */
    VkDescriptorPoolSize  pool_sizes[3];
    uint32_t              pool_size_count;
    /* What each binding was declared as, so a write of the wrong kind is
     * refused with its binding named rather than left to the driver. */
    int                   declared[AEVK_MAX_DESC];
    VkDescriptorType      desc_type[AEVK_MAX_DESC];

    /* The set pipeline_set_uniform / pipeline_set_texture write to, so code
     * that never asks for a material keeps working unchanged. */
    AevkMaterial*    def;
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

/* The surface functions, when VK_KHR_surface was enabled, and each
 * platform's create function whose extension was. A NULL entry is an
 * extension the loader did not offer, which the swapchain code reports by
 * name rather than calling through. */
static void aevk_load_surface_api(AevkInstanceApi* ia, VkInstance inst, int have_surface) {
#define AEVK_LOAD_OPT(name) ia->name = have_surface ? (PFN_##name)g_gipa(inst, #name) : NULL;
    AEVK_KHR_SURFACE_FNS(AEVK_LOAD_OPT)
#if defined(AEVK_HAVE_WIN32_SURFACE)
    AEVK_KHR_WIN32_SURFACE_FNS(AEVK_LOAD_OPT)
#endif
#if defined(AEVK_HAVE_METAL_SURFACE)
    AEVK_EXT_METAL_SURFACE_FNS(AEVK_LOAD_OPT)
#endif
#if defined(AEVK_HAVE_WAYLAND_SURFACE)
    AEVK_KHR_WAYLAND_SURFACE_FNS(AEVK_LOAD_OPT)
#endif
#if defined(AEVK_HAVE_XLIB_SURFACE)
    AEVK_KHR_XLIB_SURFACE_FNS(AEVK_LOAD_OPT)
#endif
#undef AEVK_LOAD_OPT
}

/* Creates an instance, enabling the portability enumeration extension when the
 * loader advertises it. Without it MoltenVK's device is invisible, because a
 * non-conformant implementation is hidden from a 1.0 application by default.
 *
 * Also enables VK_KHR_surface and every window-system surface extension this
 * build knows and the loader offers (#1505), and reports which window kinds
 * that makes presentable through `out_kinds` (a bit per AEVK_WINDOW_* value).
 * Enabling them costs nothing for a program that never presents, and doing
 * it here means a device is ready for a swapchain without being recreated. */
static int aevk_create_instance(AevkInstanceApi* ia, VkInstance* out, unsigned* out_kinds) {
    PFN_vkCreateInstance create =
        (PFN_vkCreateInstance)g_gipa(NULL, "vkCreateInstance");
    PFN_vkEnumerateInstanceExtensionProperties enum_ext =
        (PFN_vkEnumerateInstanceExtensionProperties)
            g_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!create || !enum_ext) {
        return aevk_fail(AEVK_ERR_NO_LOADER, "loader exports no vkCreateInstance");
    }

    const char* exts[4];
    uint32_t ext_count = 0;
    VkInstanceCreateFlags flags = 0;
    unsigned kinds = 0;
    int have_surface = 0;

    uint32_t n = 0;
    if (enum_ext(NULL, &n, NULL) == VK_SUCCESS && n) {
        VkExtensionProperties* props =
            (VkExtensionProperties*)calloc(n, sizeof(*props));
        if (!props) return aevk_fail(AEVK_ERR_OOM, "out of memory");
        if (enum_ext(NULL, &n, props) == VK_SUCCESS) {
            if (aevk_has_ext(props, n, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
                exts[ext_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
                flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            }
            if (aevk_has_ext(props, n, VK_KHR_SURFACE_EXTENSION_NAME)) {
                have_surface = 1;
                exts[ext_count++] = VK_KHR_SURFACE_EXTENSION_NAME;
#if defined(AEVK_HAVE_WIN32_SURFACE)
                if (aevk_has_ext(props, n, VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) {
                    exts[ext_count++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
                    kinds |= 1u << AEVK_WINDOW_WIN32;
                }
#elif defined(AEVK_HAVE_METAL_SURFACE)
                /* Both Apple kinds present through a CAMetalLayer: an NSView
                 * is given one, a layer is used as it is. */
                if (aevk_has_ext(props, n, VK_EXT_METAL_SURFACE_EXTENSION_NAME)) {
                    exts[ext_count++] = VK_EXT_METAL_SURFACE_EXTENSION_NAME;
                    kinds |= (1u << AEVK_WINDOW_NSVIEW) | (1u << AEVK_WINDOW_METAL_LAYER);
                }
#else
                if (aevk_has_ext(props, n, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME)) {
                    exts[ext_count++] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
                    kinds |= 1u << AEVK_WINDOW_WAYLAND;
                }
#  if defined(AEVK_HAVE_XLIB_SURFACE)
                if (aevk_has_ext(props, n, VK_KHR_XLIB_SURFACE_EXTENSION_NAME)) {
                    exts[ext_count++] = VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
                    kinds |= 1u << AEVK_WINDOW_X11;
                }
#  endif
#endif
            }
        }
        free(props);
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
    int rc = aevk_load_instance_api(ia, *out);
    if (rc != AEVK_OK) return rc;
    aevk_load_surface_api(ia, *out, have_surface);
    if (!have_surface || !ia->vkGetPhysicalDeviceSurfaceSupportKHR) kinds = 0;
    if (out_kinds) *out_kinds = kinds;
    return AEVK_OK;
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

    /* Serialised, and re-checked inside. Repeating the probe would be
     * harmless in itself, but it writes g_dev_name, a 256-byte buffer another
     * thread may be reading through device_name(), and that read could see it
     * torn. Statically initialised so there is no race to set the lock up.
     * Taken once per process in practice: the check above returns first
     * afterwards. */
    static AevkMutex probe_lock = AEVK_MUTEX_STATIC;
    AEVK_MUTEX_LOCK(&probe_lock);

    int result = 0;
    if (g_probe) {
        result = g_probe > 0;
    } else {
        /* Probe by actually creating an instance and enumerating: a loader
         * with no ICD behind it exports every symbol and still cannot
         * render. */
        g_probe = -1;
        if (aevk_load_library() == AEVK_OK) {
            AevkInstanceApi ia;
            VkInstance inst = VK_NULL_HANDLE;
            if (aevk_create_instance(&ia, &inst, NULL) == AEVK_OK) {
                VkPhysicalDevice phys;
                uint32_t family;
                result = aevk_pick_physical(&ia, inst, &phys, &family,
                                            g_dev_name, sizeof(g_dev_name)) == AEVK_OK;
                ia.vkDestroyInstance(inst, NULL);
                if (result) { g_probe = 1; aevk_clear_error(); }
            }
        }
    }

    AEVK_MUTEX_UNLOCK(&probe_lock);
    return result;
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
    AEVK_MUTEX_INIT(&d->lock);

    if (aevk_create_instance(&d->ia, &d->instance, &d->surface_kinds) != AEVK_OK) goto fail;
    if (aevk_pick_physical(&d->ia, d->instance, &d->phys, &d->queue_family,
                           d->name, sizeof(d->name)) != AEVK_OK) goto fail;

    VkPhysicalDeviceProperties props;
    d->ia.vkGetPhysicalDeviceProperties(d->phys, &props);
    d->max_dim = props.limits.maxImageDimension2D;
    d->max_groups[0] = props.limits.maxComputeWorkGroupCount[0];
    d->max_groups[1] = props.limits.maxComputeWorkGroupCount[1];
    d->max_groups[2] = props.limits.maxComputeWorkGroupCount[2];
    d->sample_counts = props.limits.framebufferColorSampleCounts &
                       props.limits.framebufferDepthSampleCounts;
    d->ia.vkGetPhysicalDeviceMemoryProperties(d->phys, &d->mem_props);

    /* VK_KHR_portability_subset must be enabled when the device advertises it,
     * or vkCreateDevice is required to fail. This is the MoltenVK path.
     * VK_KHR_swapchain is enabled whenever the instance can make a surface
     * and the device offers it, so the same device renders offscreen and
     * presents (#1505). */
    const char* dev_exts[2];
    uint32_t dev_ext_count = 0;
    int want_swapchain = 0;
    uint32_t en = 0;
    if (d->ia.vkEnumerateDeviceExtensionProperties(d->phys, NULL, &en, NULL) == VK_SUCCESS && en) {
        VkExtensionProperties* eps = (VkExtensionProperties*)calloc(en, sizeof(*eps));
        if (!eps) { aevk_fail(AEVK_ERR_OOM, "out of memory"); goto fail; }
        if (d->ia.vkEnumerateDeviceExtensionProperties(d->phys, NULL, &en, eps) == VK_SUCCESS) {
            if (aevk_has_ext(eps, en, "VK_KHR_portability_subset")) {
                dev_exts[dev_ext_count++] = "VK_KHR_portability_subset";
            }
            if (d->surface_kinds && aevk_has_ext(eps, en, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                dev_exts[dev_ext_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
                want_swapchain = 1;
            }
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
    if (want_swapchain) {
#define AEVK_LOAD_SC(name) d->da.name = (PFN_##name)d->ia.vkGetDeviceProcAddr(d->device, #name);
        AEVK_KHR_SWAPCHAIN_FNS(AEVK_LOAD_SC)
#undef AEVK_LOAD_SC
        d->can_present = d->da.vkCreateSwapchainKHR && d->da.vkDestroySwapchainKHR &&
                         d->da.vkGetSwapchainImagesKHR && d->da.vkAcquireNextImageKHR &&
                         d->da.vkQueuePresentKHR;
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
    /* Destroying a device concurrently with anything using it is the caller's
     * error, not something a lock can rescue: the lock is inside the object
     * being freed. The contract is in the README. */
    AEVK_MUTEX_DESTROY(&d->lock);
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

/* A depth format the device supports for optimal-tiling depth attachments,
 * preferring plain depth over combined depth+stencil: a caller who asked only
 * for depth should not pay for a stencil it never reads. VK_FORMAT_UNDEFINED
 * when the device offers none, which is possible in principle and worth
 * reporting rather than assuming. */
static VkFormat aevk_pick_depth_format(AevkDevice* d) {
    static const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D16_UNORM,
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        VkFormatProperties fp;
        d->ia.vkGetPhysicalDeviceFormatProperties(d->phys, candidates[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return candidates[i];
        }
    }
    return VK_FORMAT_UNDEFINED;
}

static VkSampleCountFlagBits aevk_sample_bit(int samples) {
    switch (samples) {
        case 1:  return VK_SAMPLE_COUNT_1_BIT;
        case 2:  return VK_SAMPLE_COUNT_2_BIT;
        case 4:  return VK_SAMPLE_COUNT_4_BIT;
        case 8:  return VK_SAMPLE_COUNT_8_BIT;
        case 16: return VK_SAMPLE_COUNT_16_BIT;
        default: return (VkSampleCountFlagBits)0;
    }
}

/* Creates an image plus its memory and view in one step: the colour, resolve
 * and depth attachments differ only in format, usage and aspect. */
static int aevk_make_attachment(AevkDevice* d, int width, int height,
                                VkFormat format, VkSampleCountFlagBits samples,
                                VkImageUsageFlags usage, VkImageAspectFlags aspect,
                                VkImage* out_img, VkDeviceMemory* out_mem,
                                VkImageView* out_view) {
    VkImageCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent.width = (uint32_t)width;
    ici.extent.height = (uint32_t)height;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = samples;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult r = d->da.vkCreateImage(d->device, &ici, NULL, out_img);
    if (r != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkCreateImage failed (%d)", (int)r);

    VkMemoryRequirements req;
    d->da.vkGetImageMemoryRequirements(d->device, *out_img, &req);
    uint32_t type = 0;
    if (aevk_find_memory(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                         &type) != AEVK_OK &&
        aevk_find_memory(d, req.memoryTypeBits, 0, &type) != AEVK_OK) {
        return aevk_fail(AEVK_ERR_UNSUPPORTED, "no memory type for an attachment");
    }
    VkMemoryAllocateInfo mi = {0};
    mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = type;
    r = d->da.vkAllocateMemory(d->device, &mi, NULL, out_mem);
    if (r != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkAllocateMemory failed (%d)", (int)r);
    r = d->da.vkBindImageMemory(d->device, *out_img, *out_mem, 0);
    if (r != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkBindImageMemory failed (%d)", (int)r);

    VkImageViewCreateInfo vci = {0};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = *out_img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange.aspectMask = aspect;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    r = d->da.vkCreateImageView(d->device, &vci, NULL, out_view);
    if (r != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkCreateImageView failed (%d)", (int)r);
    return AEVK_OK;
}

/* Frees every per-frame slot. THE DEVICE LOCK MUST BE HELD, and the device
 * must already be idle: the caller either just waited or is tearing the target
 * down. */
static void aevk_frames_free(AevkTarget* t) {
    if (!t || !t->frames) return;
    AevkDevice* d = t->dev;
    for (int i = 0; i < t->frame_count; i++) {
        AevkFrame* f = &t->frames[i];
        if (f->fence)        d->da.vkDestroyFence(d->device, f->fence, NULL);
        if (f->cmd)          d->da.vkFreeCommandBuffers(d->device, d->pool, 1, &f->cmd);
        if (f->readback_ptr) d->da.vkUnmapMemory(d->device, f->readback_mem);
        if (f->readback)     d->da.vkDestroyBuffer(d->device, f->readback, NULL);
        if (f->readback_mem) d->da.vkFreeMemory(d->device, f->readback_mem, NULL);
    }
    free(t->frames);
    t->frames = NULL;
    t->frame_count = 0;
    t->next_frame = 0;
    t->last_done = 0;
    t->last_submitted = -1;
}

/* Allocates `count` slots, each with its own command buffer, fence and
 * readback buffer. One slot is the synchronous shape and the default; the
 * extra ones cost a readback buffer each, which is why they are opt-in.
 *
 * THE DEVICE LOCK MUST BE HELD. It touches the command pool, and taking the
 * lock here instead would self-deadlock set_frames, which holds it across the
 * free-and-reallocate so no other thread can submit into a half-built set.
 * Same rule as every other helper in this file: the public entry point locks,
 * the helpers do not. */
static int aevk_frames_alloc(AevkTarget* t, int count) {
    AevkDevice* d = t->dev;
    AevkFrame* frames = (AevkFrame*)calloc((size_t)count, sizeof(AevkFrame));
    if (!frames) return aevk_fail(AEVK_ERR_OOM, "out of memory");

    for (int i = 0; i < count; i++) {
        AevkFrame* f = &frames[i];
        VkResult r;
        /* A target whose readback is off never copies a frame out, so its
         * slots carry no readback buffer at all. */
        if (t->readback_on) {
            if (aevk_make_buffer(d, t->readback_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 &f->readback, &f->readback_mem) != AEVK_OK) {
                goto fail;
            }
            /* Mapped once and left mapped: a per-frame map/unmap pair is a
             * driver round trip that buys nothing for a buffer that lives this
             * long. */
            r = d->da.vkMapMemory(d->device, f->readback_mem, 0,
                                  t->readback_size, 0, &f->readback_ptr);
            if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkMapMemory failed (%d)", (int)r); goto fail; }
        }

        VkCommandBufferAllocateInfo cai = {0};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = d->pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        r = d->da.vkAllocateCommandBuffers(d->device, &cai, &f->cmd);
        if (r != VK_SUCCESS) {
            aevk_fail(AEVK_ERR_OOM, "vkAllocateCommandBuffers failed (%d)", (int)r);
            goto fail;
        }

        VkFenceCreateInfo fnci = {0};
        fnci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        r = d->da.vkCreateFence(d->device, &fnci, NULL, &f->fence);
        if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkCreateFence failed (%d)", (int)r); goto fail; }
    }

    t->frames = frames;
    t->frame_count = count;
    t->next_frame = 0;
    t->last_done = 0;
    t->last_submitted = -1;
    return AEVK_OK;

fail:
    /* Hand the partial array to the normal teardown by installing it first:
     * a hand-rolled unwind here would be a second thing to keep correct. */
    t->frames = frames;
    t->frame_count = count;
    aevk_frames_free(t);
    return AEVK_ERR_OOM;
}

/* The colour image the target renders into and reads back from, the
 * multisampled and depth attachments when the target has them, and the
 * framebuffer over them: everything that depends on the SIZE. target_resize
 * rebuilds exactly this and keeps the render pass, which depends only on the
 * formats and the sample count, and with it every pipeline made for the
 * target. The caller guarantees nothing is using the old images. */
static int aevk_target_make_images(AevkTarget* t) {
    AevkDevice* d = t->dev;
    int rc = aevk_make_attachment(d, t->width, t->height, t->color_format,
                                  VK_SAMPLE_COUNT_1_BIT,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT,
                                  &t->image, &t->image_mem, &t->view);
    if (rc != AEVK_OK) return rc;

    /* The multisampled colour image is TRANSIENT: it exists only inside the
     * render pass, resolving into `image`, so a tiler never has to write it
     * to memory at all. */
    if (t->samples > 1) {
        rc = aevk_make_attachment(d, t->width, t->height, t->color_format,
                                  aevk_sample_bit(t->samples),
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT,
                                  &t->msaa_image, &t->msaa_mem, &t->msaa_view);
        if (rc != AEVK_OK) return rc;
    }
    if (t->has_depth) {
        rc = aevk_make_attachment(d, t->width, t->height, t->depth_format,
                                  aevk_sample_bit(t->samples),
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                  VK_IMAGE_ASPECT_DEPTH_BIT,
                                  &t->depth_image, &t->depth_mem, &t->depth_view);
        if (rc != AEVK_OK) return rc;
    }

    /* In attachment order, which the render pass fixed: [0] colour written,
     * [1] resolve (MSAA only), [last] depth. */
    VkImageView views[3];
    uint32_t n_view = 0;
    views[n_view++] = (t->samples > 1) ? t->msaa_view : t->view;
    if (t->samples > 1) views[n_view++] = t->view;
    if (t->has_depth)   views[n_view++] = t->depth_view;

    VkFramebufferCreateInfo fci = {0};
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fci.renderPass = t->pass;
    fci.attachmentCount = n_view;
    fci.pAttachments = views;
    fci.width = (uint32_t)t->width;
    fci.height = (uint32_t)t->height;
    fci.layers = 1;
    VkResult r = d->da.vkCreateFramebuffer(d->device, &fci, NULL, &t->fb);
    if (r != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkCreateFramebuffer failed (%d)", (int)r);
    return AEVK_OK;
}

/* Releases what aevk_target_make_images made, including a partial set left
 * by a failure part-way through it. */
static void aevk_target_free_images(AevkTarget* t) {
    AevkDevice* d = t->dev;
    if (t->fb)          d->da.vkDestroyFramebuffer(d->device, t->fb, NULL);
    if (t->view)        d->da.vkDestroyImageView(d->device, t->view, NULL);
    if (t->image)       d->da.vkDestroyImage(d->device, t->image, NULL);
    if (t->image_mem)   d->da.vkFreeMemory(d->device, t->image_mem, NULL);
    if (t->msaa_view)   d->da.vkDestroyImageView(d->device, t->msaa_view, NULL);
    if (t->msaa_image)  d->da.vkDestroyImage(d->device, t->msaa_image, NULL);
    if (t->msaa_mem)    d->da.vkFreeMemory(d->device, t->msaa_mem, NULL);
    if (t->depth_view)  d->da.vkDestroyImageView(d->device, t->depth_view, NULL);
    if (t->depth_image) d->da.vkDestroyImage(d->device, t->depth_image, NULL);
    if (t->depth_mem)   d->da.vkFreeMemory(d->device, t->depth_mem, NULL);
    t->fb = VK_NULL_HANDLE;
    t->view = VK_NULL_HANDLE;
    t->image = VK_NULL_HANDLE;
    t->image_mem = VK_NULL_HANDLE;
    t->msaa_view = VK_NULL_HANDLE;
    t->msaa_image = VK_NULL_HANDLE;
    t->msaa_mem = VK_NULL_HANDLE;
    t->depth_view = VK_NULL_HANDLE;
    t->depth_image = VK_NULL_HANDLE;
    t->depth_mem = VK_NULL_HANDLE;
    t->rendered = 0;
}

/* The render pass: attachment formats, sample count and layouts, nothing
 * that depends on the size. */
static int aevk_target_make_pass(AevkTarget* t) {
    AevkDevice* d = t->dev;

    /* Attachment 0 is always the one the draw writes: the multisampled image
     * when there is one, otherwise the single-sample image that gets copied
     * out. finalLayout TRANSFER_SRC_OPTIMAL on whichever ends up being copied,
     * so the copy after the render pass needs no barrier, and presenting
     * reads it from the same layout.
     *
     * Order: [0] colour written, [1] resolve (MSAA only), [last] depth. The
     * clear-value array is indexed by attachment, so record() has to build it
     * in this same order. */
    VkAttachmentDescription atts[3];
    memset(atts, 0, sizeof(atts));
    uint32_t n_att = 0;

    uint32_t color_index = n_att;
    atts[n_att].format = t->color_format;
    atts[n_att].samples = aevk_sample_bit(t->samples);
    atts[n_att].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[n_att].storeOp = (t->samples > 1) ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                           : VK_ATTACHMENT_STORE_OP_STORE;
    atts[n_att].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[n_att].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[n_att].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[n_att].finalLayout = (t->samples > 1) ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                               : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    n_att++;

    uint32_t resolve_index = 0;
    if (t->samples > 1) {
        resolve_index = n_att;
        atts[n_att].format = t->color_format;
        atts[n_att].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[n_att].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[n_att].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[n_att].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[n_att].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[n_att].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[n_att].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        n_att++;
    }

    uint32_t depth_index = 0;
    if (t->has_depth) {
        depth_index = n_att;
        atts[n_att].format = t->depth_format;
        atts[n_att].samples = aevk_sample_bit(t->samples);
        atts[n_att].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[n_att].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[n_att].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[n_att].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[n_att].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[n_att].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        n_att++;
    }
    t->clear_count = n_att;
    t->depth_clear_index = t->has_depth ? depth_index : 0;

    VkAttachmentReference color_ref = {0};
    color_ref.attachment = color_index;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference resolve_ref = {0};
    resolve_ref.attachment = resolve_index;
    resolve_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depth_ref = {0};
    depth_ref.attachment = depth_index;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color_ref;
    if (t->samples > 1)  sub.pResolveAttachments = &resolve_ref;
    if (t->has_depth)    sub.pDepthStencilAttachment = &depth_ref;

    /* deps[0] orders this frame's attachment writes (and the layout
     * transition out of UNDEFINED) after everything earlier on the queue that
     * touched the same images: the previous frame's attachment writes, and
     * the transfer reads of its readback copy and of a present blit. With a
     * single frame in flight the fence wait already separates them; with
     * several, or with a swapchain reading the image, only this dependency
     * does. */
    VkSubpassDependency deps[2] = {{0}, {0}};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo rpi = {0};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = n_att;
    rpi.pAttachments = atts;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 2;
    rpi.pDependencies = deps;
    VkResult r = d->da.vkCreateRenderPass(d->device, &rpi, NULL, &t->pass);
    if (r != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkCreateRenderPass failed (%d)", (int)r);
    return AEVK_OK;
}

/* The colour formats a target can be created with (#1514): 8-bit UNORM,
 * which is what a display shows as written; 8-bit sRGB, whose stores encode
 * linear shader output for display; and half and single float, for HDR or
 * compute-style output that must not be clamped to 0..1. Bytes a pixel, or 0
 * for a format this module does not render to. */
static int aevk_format_bpp(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:        return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT:  return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT:  return 16;
        default:                             return 0;
    }
}

/* IEEE 754 half to float, subnormals, infinities and NaN included. */
static float aevk_half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            /* Subnormal: shift the mantissa up until it is normalised. */
            exp = 127 - 15 + 1;
            while (!(mant & 0x400u)) { mant <<= 1; exp--; }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

/* Channel `c` (0 red .. 3 alpha) of the pixel at `px`, in the target's
 * format, as a float: the stored byte over 255 for the 8-bit formats (for
 * sRGB that is the ENCODED value, which is what is stored and displayed), the
 * value itself for the float ones. */
static float aevk_channel_value(const AevkTarget* t, const unsigned char* px, int c) {
    switch (t->color_format) {
        case VK_FORMAT_R16G16B16A16_SFLOAT: {
            uint16_t h;
            memcpy(&h, px + c * 2, sizeof(h));
            return aevk_half_to_float(h);
        }
        case VK_FORMAT_R32G32B32A32_SFLOAT: {
            float f;
            memcpy(&f, px + c * 4, sizeof(f));
            return f;
        }
        default:
            return (float)px[c] / 255.0f;
    }
}

/* The 8-bit value of channel `c`: the stored byte for the 8-bit formats, the
 * float clamped to 0..1 and rounded for the others. NaN reads as 0. */
static unsigned char aevk_channel_u8(const AevkTarget* t, const unsigned char* px, int c) {
    if (t->bytes_per_pixel == 4) return px[c];
    float v = aevk_channel_value(t, px, c);
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    return (unsigned char)(v * 255.0f + 0.5f);
}

/* Size checks shared by create and resize: positive, within the device's
 * image limit, and a readback that fits in host memory. The byte count is
 * computed in 64-bit so a large target reports UNSUPPORTED rather than
 * wrapping to a small buffer. */
static int aevk_target_check_size(AevkDevice* d, int width, int height, int bpp,
                                  uint64_t* out_bytes) {
    if (width <= 0 || height <= 0) {
        return aevk_fail(AEVK_ERR_ARG, "size must be positive, got %dx%d", width, height);
    }
    if ((uint32_t)width > d->max_dim || (uint32_t)height > d->max_dim) {
        return aevk_fail(AEVK_ERR_UNSUPPORTED, "%dx%d exceeds the device limit of %u",
                         width, height, d->max_dim);
    }
    uint64_t bytes = (uint64_t)width * (uint64_t)height * (uint64_t)bpp;
    if (bytes > (uint64_t)SIZE_MAX || bytes > (uint64_t)0x7fffffff) {
        return aevk_fail(AEVK_ERR_UNSUPPORTED, "%dx%d does not fit in host memory", width, height);
    }
    *out_bytes = bytes;
    return AEVK_OK;
}

AevkTarget* aevk_target_create(AevkDevice* d, int width, int height) {
    return aevk_target_create_format(d, width, height, VK_FORMAT_R8G8B8A8_UNORM, 0, 1);
}

AevkTarget* aevk_target_create_ex(AevkDevice* d, int width, int height,
                                  int want_depth, int samples) {
    return aevk_target_create_format(d, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                                     want_depth, samples);
}

AevkTarget* aevk_target_create_format(AevkDevice* d, int width, int height, int format,
                                      int want_depth, int samples) {
    aevk_clear_error();
    if (!d) { aevk_fail(AEVK_ERR_ARG, "device is null"); return NULL; }
    int bpp = aevk_format_bpp((VkFormat)format);
    if (!bpp) {
        aevk_fail(AEVK_ERR_ARG,
                  "format %d is not a target format (R8G8B8A8_UNORM 37, R8G8B8A8_SRGB 43, "
                  "R16G16B16A16_SFLOAT 97, R32G32B32A32_SFLOAT 109)", format);
        return NULL;
    }
    /* Asked of the device rather than assumed: rendering to it is the
     * attachment feature. The float formats' support is near universal on
     * desktop drivers and not guaranteed everywhere. */
    VkFormatProperties fp;
    d->ia.vkGetPhysicalDeviceFormatProperties(d->phys, (VkFormat)format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
        aevk_fail(AEVK_ERR_UNSUPPORTED, "the device cannot render to format %d", format);
        return NULL;
    }
    uint64_t bytes = 0;
    if (aevk_target_check_size(d, width, height, bpp, &bytes) != AEVK_OK) return NULL;

    VkSampleCountFlagBits sample_bit = aevk_sample_bit(samples);
    if (!sample_bit) {
        aevk_fail(AEVK_ERR_ARG, "sample count must be 1, 2, 4, 8 or 16 (got %d)", samples);
        return NULL;
    }
    if (samples > 1 && !(d->sample_counts & sample_bit)) {
        aevk_fail(AEVK_ERR_UNSUPPORTED,
                  "device does not support %dx multisampling for framebuffers", samples);
        return NULL;
    }

    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    if (want_depth) {
        depth_format = aevk_pick_depth_format(d);
        if (depth_format == VK_FORMAT_UNDEFINED) {
            aevk_fail(AEVK_ERR_UNSUPPORTED, "device offers no depth attachment format");
            return NULL;
        }
    }

    AevkTarget* t = (AevkTarget*)calloc(1, sizeof(*t));
    if (!t) { aevk_fail(AEVK_ERR_OOM, "out of memory"); return NULL; }
    t->dev = d;
    t->width = width;
    t->height = height;
    t->color_format = (VkFormat)format;
    t->bytes_per_pixel = bpp;
    t->readback_on = 1;
    t->readback_size = (VkDeviceSize)bytes;
    t->samples = samples;
    t->index_bits = 32;
    t->has_depth = want_depth ? 1 : 0;
    t->depth_format = depth_format;
    t->timeout_ns = 5000000000ull;

    if (aevk_target_make_pass(t) != AEVK_OK) goto fail;
    if (aevk_target_make_images(t) != AEVK_OK) goto fail;

    AEVK_MUTEX_LOCK(&d->lock);
    int frames_rc = aevk_frames_alloc(t, 1);
    AEVK_MUTEX_UNLOCK(&d->lock);
    if (frames_rc != AEVK_OK) goto fail;

    return t;

fail:
    aevk_target_destroy(t);
    return NULL;
}

void aevk_target_destroy(AevkTarget* t) {
    if (!t) return;
    AevkDevice* d = t->dev;
    if (d && d->device) {
        AEVK_MUTEX_LOCK(&d->lock);
        d->da.vkDeviceWaitIdle(d->device);
        aevk_frames_free(t);
        if (t->vbuf_ptr)     d->da.vkUnmapMemory(d->device, t->vbuf_mem);
        if (t->vbuf)         d->da.vkDestroyBuffer(d->device, t->vbuf, NULL);
        if (t->vbuf_mem)     d->da.vkFreeMemory(d->device, t->vbuf_mem, NULL);
        if (t->ibuf_ptr)     d->da.vkUnmapMemory(d->device, t->ibuf_mem);
        if (t->ibuf)         d->da.vkDestroyBuffer(d->device, t->ibuf, NULL);
        if (t->ibuf_mem)     d->da.vkFreeMemory(d->device, t->ibuf_mem, NULL);
        aevk_target_free_images(t);
        if (t->pass)         d->da.vkDestroyRenderPass(d->device, t->pass, NULL);
        AEVK_MUTEX_UNLOCK(&d->lock);
    }
    free(t->batch);
    free(t);
}

/* A window changed size: new images and framebuffer at the new size, the
 * same render pass, so every pipeline made for the target stays valid.
 * Geometry, push constants, the batch and the frame count carry over; the
 * previous contents do not, and nothing can be presented until a frame has
 * been drawn at the new size. On failure the target has no images and every
 * draw is refused until a resize succeeds. */
int aevk_target_resize(AevkTarget* t, int width, int height) {
    aevk_clear_error();
    if (!t) return aevk_fail(AEVK_ERR_ARG, "target is null");
    AevkDevice* d = t->dev;
    uint64_t bytes = 0;
    int rc = aevk_target_check_size(d, width, height, t->bytes_per_pixel, &bytes);
    if (rc != AEVK_OK) return rc;
    if (width == t->width && height == t->height && t->fb) return AEVK_OK;

    AEVK_MUTEX_LOCK(&d->lock);
    d->da.vkDeviceWaitIdle(d->device);
    int frames = t->frame_count > 0 ? t->frame_count : 1;
    aevk_frames_free(t);
    aevk_target_free_images(t);
    t->width = width;
    t->height = height;
    t->readback_size = (VkDeviceSize)bytes;
    rc = aevk_target_make_images(t);
    if (rc == AEVK_OK) rc = aevk_frames_alloc(t, frames);
    if (rc != AEVK_OK) {
        /* Leave nothing half-built behind: a target without images is one
         * every draw refuses, which is what the comment above promises. */
        aevk_frames_free(t);
        aevk_target_free_images(t);
    }
    AEVK_MUTEX_UNLOCK(&d->lock);
    return rc;
}

/* Whether each frame is copied into host memory for pixel(), copy_rgba() and
 * save_ppm(). A target that is only ever presented turns it off: the copy is
 * width*height*4 bytes of transfer per frame and a readback buffer per frame
 * slot, all for pixels nobody reads. */
int aevk_target_set_readback(AevkTarget* t, int on) {
    aevk_clear_error();
    if (!t) return aevk_fail(AEVK_ERR_ARG, "target is null");
    on = on ? 1 : 0;
    if (on == t->readback_on) return AEVK_OK;
    AevkDevice* d = t->dev;
    AEVK_MUTEX_LOCK(&d->lock);
    d->da.vkDeviceWaitIdle(d->device);
    int frames = t->frame_count > 0 ? t->frame_count : 1;
    aevk_frames_free(t);
    t->readback_on = on;
    int rc = t->fb ? aevk_frames_alloc(t, frames) : AEVK_OK;
    AEVK_MUTEX_UNLOCK(&d->lock);
    return rc;
}

int aevk_target_readback(const AevkTarget* t) { return t ? t->readback_on : 0; }

int aevk_target_format(const AevkTarget* t) { return t ? (int)t->color_format : 0; }
int aevk_target_bytes_per_pixel(const AevkTarget* t) { return t ? t->bytes_per_pixel : 0; }

/* Draw batching (#1540). An empty batch is the default and draws all the
 * geometry once, which is every caller that never asks for one. */
int aevk_batch_reset(AevkTarget* t) {
    aevk_clear_error();
    if (!t) return aevk_fail(AEVK_ERR_ARG, "target is null");
    AEVK_MUTEX_LOCK(&t->dev->lock);
    t->batch_count = 0;
    t->batch_version++;
    AEVK_MUTEX_UNLOCK(&t->dev->lock);
    return AEVK_OK;
}

int aevk_batch_add(AevkTarget* t, AevkMaterial* mat, int first, int count) {
    aevk_clear_error();
    if (!t) return aevk_fail(AEVK_ERR_ARG, "target is null");
    if (first < 0) return aevk_fail(AEVK_ERR_ARG, "first must not be negative, got %d", first);
    if (count <= 0) return aevk_fail(AEVK_ERR_ARG, "draw count must be positive, got %d", count);

    AEVK_MUTEX_LOCK(&t->dev->lock);
    /* Checked here against the geometry uploaded so far, so a mistake is
     * reported where it was made. A batch built before any geometry is legal
     * and is caught at draw time instead. */
    int limit = t->index_count > 0 ? t->index_count : t->vertex_count;
    if (limit > 0 && (long long)first + (long long)count > (long long)limit) {
        AEVK_MUTEX_UNLOCK(&t->dev->lock);
        return aevk_fail(AEVK_ERR_ARG, "draw covers %d..%lld but only %d are uploaded",
                         first, (long long)first + count - 1, limit);
    }
    if (t->batch_count == t->batch_cap) {
        int cap = t->batch_cap ? t->batch_cap * 2 : 8;
        AevkDrawItem* grown = (AevkDrawItem*)realloc(t->batch, (size_t)cap * sizeof(*grown));
        if (!grown) {
            AEVK_MUTEX_UNLOCK(&t->dev->lock);
            return aevk_fail(AEVK_ERR_OOM, "out of memory");
        }
        t->batch = grown;
        t->batch_cap = cap;
    }
    t->batch[t->batch_count].mat = mat;
    t->batch[t->batch_count].first = first;
    t->batch[t->batch_count].count = count;
    t->batch_count++;
    t->batch_version++;
    AEVK_MUTEX_UNLOCK(&t->dev->lock);
    return AEVK_OK;
}

int aevk_batch_count(const AevkTarget* t) { return t ? t->batch_count : 0; }

/* Ranges are checked here rather than at add time: the geometry a batch
 * slices can be re-uploaded between adding and drawing, so the count that
 * matters is the one in effect for THIS frame. */
static int aevk_batch_check(AevkTarget* t, AevkPipeline* p) {
    int limit = t->index_count > 0 ? t->index_count : t->vertex_count;
    const char* what = t->index_count > 0 ? "indices" : "vertices";
    for (int i = 0; i < t->batch_count; i++) {
        AevkDrawItem* it = &t->batch[i];
        if ((long long)it->first + (long long)it->count > (long long)limit) {
            return aevk_fail(AEVK_ERR_ARG,
                             "draw %d covers %s %d..%lld but only %d are uploaded",
                             i, what, it->first, (long long)it->first + it->count - 1, limit);
        }
        if (it->mat && p && it->mat->pipe != p) {
            return aevk_fail(AEVK_ERR_ARG, "draw %d uses a material of another pipeline", i);
        }
    }
    return AEVK_OK;
}

int aevk_target_has_depth(const AevkTarget* t) { return t ? t->has_depth : 0; }
int aevk_target_samples(const AevkTarget* t)   { return t ? t->samples : 0; }

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

/* A storage buffer: `layout(std430, binding = N) buffer`. What a compute pass
 * reads and writes, and what a vertex shader pulls computed data from. */
int aevk_bindings_storage(AevkBindings* b, int binding) {
    aevk_clear_error();
    return aevk_bindings_add(b, binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
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

static void aevk_image_barrier_levels(AevkDevice* d, VkCommandBuffer cmd, VkImage img,
                                      uint32_t base_level, uint32_t level_count,
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
    b.subresourceRange.baseMipLevel = base_level;
    b.subresourceRange.levelCount = level_count;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    d->da.vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
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

/* How many mip levels a full chain needs: halving until 1x1. */
static uint32_t aevk_mip_levels_for(int width, int height) {
    uint32_t levels = 1;
    int w = width, h = height;
    while (w > 1 || h > 1) {
        w = (w > 1) ? w / 2 : 1;
        h = (h > 1) ? h / 2 : 1;
        levels++;
    }
    return levels;
}

AevkTexture* aevk_texture_create(AevkDevice* d, int width, int height) {
    return aevk_texture_create_ex(d, width, height, 0, 0, 0);
}

/* `mipmapped` builds a full mip chain, which needs the device to support
 * linear blitting of the format. `linear_filter` picks LINEAR over NEAREST for
 * magnification, minification and between mip levels. `repeat` picks REPEAT
 * over CLAMP_TO_EDGE for addressing. */
AevkTexture* aevk_texture_create_ex(AevkDevice* d, int width, int height,
                                    int mipmapped, int linear_filter, int repeat) {
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

    uint32_t levels = 1;
    if (mipmapped) {
        /* Generating the chain is a chain of blits, and vkCmdBlitImage names
         * three format features as required: BLIT_SRC on the source, BLIT_DST
         * on the destination (the same image, different levels), and
         * SAMPLED_IMAGE_FILTER_LINEAR on the source for VK_FILTER_LINEAR.
         * Checking only the filter bit, as this did, left the blit itself
         * unchecked. Refusing beats generating a black chain nobody notices. */
        VkFormatProperties fp;
        d->ia.vkGetPhysicalDeviceFormatProperties(d->phys, VK_FORMAT_R8G8B8A8_UNORM, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                          VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((fp.optimalTilingFeatures & need) != need) {
            aevk_fail(AEVK_ERR_UNSUPPORTED,
                      "device cannot linear-blit R8G8B8A8_UNORM (features 0x%x), "
                      "so it cannot build mipmaps",
                      (unsigned)fp.optimalTilingFeatures);
            return NULL;
        }
        levels = aevk_mip_levels_for(width, height);
    }

    AevkTexture* tex = (AevkTexture*)calloc(1, sizeof(*tex));
    if (!tex) { aevk_fail(AEVK_ERR_OOM, "out of memory"); return NULL; }
    tex->dev = d;
    tex->width = width;
    tex->height = height;
    tex->mip_levels = levels;

    VkImageCreateInfo ii = {0};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width = (uint32_t)width;
    ii.extent.height = (uint32_t)height;
    ii.extent.depth = 1;
    ii.mipLevels = levels;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    /* TRANSFER_SRC as well as DST: building the chain blits level N-1 into
     * level N, so the image reads from itself. */
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
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
    vi.subresourceRange.levelCount = levels;
    vi.subresourceRange.layerCount = 1;
    r = d->da.vkCreateImageView(d->device, &vi, NULL, &tex->view);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkCreateImageView failed (%d)", (int)r); goto fail; }

    VkSamplerCreateInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    VkFilter filter = linear_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    VkSamplerAddressMode mode = repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                       : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.magFilter = filter;
    si.minFilter = filter;
    si.mipmapMode = linear_filter ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                  : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = mode;
    si.addressModeV = mode;
    si.addressModeW = mode;
    si.maxLod = (float)levels;
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    r = d->da.vkCreateSampler(d->device, &si, NULL, &tex->sampler);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkCreateSampler failed (%d)", (int)r); goto fail; }

    return tex;
fail:
    aevk_texture_destroy(tex);
    return NULL;
}

int aevk_texture_mip_levels(const AevkTexture* tex) {
    return tex ? (int)tex->mip_levels : 0;
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

    /* run_once allocates from the device pool and submit_once uses the
     * device queue, so both are inside the lock rather than taking it
     * themselves: texture_upload composes them and a self-taking helper
     * would deadlock. */
    AEVK_MUTEX_LOCK(&d->lock);
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    rc = aevk_run_once(d, &cmd);
    if (rc == AEVK_OK) {
        aevk_image_barrier_levels(d, cmd, tex->image, 0, tex->mip_levels,
                                  VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkBufferImageCopy copy = {0};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = (uint32_t)tex->width;
        copy.imageExtent.height = (uint32_t)tex->height;
        copy.imageExtent.depth = 1;
        d->da.vkCmdCopyBufferToImage(cmd, staging, tex->image,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        if (tex->mip_levels > 1) {
            /* Each level is blitted from the one above, so level N-1 has to be
             * readable before level N is written. Walking the chain one level
             * at a time is what keeps that ordering explicit. */
            int mw = tex->width, mh = tex->height;
            for (uint32_t level = 1; level < tex->mip_levels; level++) {
                aevk_image_barrier_levels(d, cmd, tex->image, level - 1, 1,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                          VK_ACCESS_TRANSFER_WRITE_BIT,
                                          VK_ACCESS_TRANSFER_READ_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT);

                int nw = (mw > 1) ? mw / 2 : 1;
                int nh = (mh > 1) ? mh / 2 : 1;
                VkImageBlit blit = {0};
                blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.srcSubresource.mipLevel = level - 1;
                blit.srcSubresource.layerCount = 1;
                blit.srcOffsets[1].x = mw;
                blit.srcOffsets[1].y = mh;
                blit.srcOffsets[1].z = 1;
                blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.dstSubresource.mipLevel = level;
                blit.dstSubresource.layerCount = 1;
                blit.dstOffsets[1].x = nw;
                blit.dstOffsets[1].y = nh;
                blit.dstOffsets[1].z = 1;
                d->da.vkCmdBlitImage(cmd,
                                     tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     1, &blit, VK_FILTER_LINEAR);

                aevk_image_barrier_levels(d, cmd, tex->image, level - 1, 1,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                          VK_ACCESS_TRANSFER_READ_BIT,
                                          VK_ACCESS_SHADER_READ_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                mw = nw;
                mh = nh;
            }
            /* The last level was never blitted from, so it is still DST. */
            aevk_image_barrier_levels(d, cmd, tex->image, tex->mip_levels - 1, 1,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        } else {
            aevk_image_barrier(d, cmd, tex->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        rc = aevk_submit_once(d, cmd);
    }
    AEVK_MUTEX_UNLOCK(&d->lock);

    d->da.vkDestroyBuffer(d->device, staging, NULL);
    d->da.vkFreeMemory(d->device, staging_mem, NULL);
    if (rc == AEVK_OK) tex->uploaded = 1;
    return rc;
}

/* Adds a pool. Called when the newest one is full, or for the first set. */
static int aevk_pipeline_add_pool(AevkPipeline* p) {
    AevkDevice* d = p->dev;
    if (p->pool_count >= AEVK_MAX_POOLS) {
        return aevk_fail(AEVK_ERR_OOM, "at most %d descriptor pools per pipeline (%d materials)",
                         AEVK_MAX_POOLS, AEVK_MAX_POOLS * AEVK_SETS_PER_POOL);
    }
    VkDescriptorPoolCreateInfo dpi = {0};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = AEVK_SETS_PER_POOL;
    dpi.poolSizeCount = p->pool_size_count;
    dpi.pPoolSizes = p->pool_sizes;
    VkResult r = d->da.vkCreateDescriptorPool(d->device, &dpi, NULL, &p->pools[p->pool_count]);
    if (r != VK_SUCCESS) {
        return aevk_fail(AEVK_ERR_OOM, "vkCreateDescriptorPool failed (%d)", (int)r);
    }
    p->pool_count++;
    p->sets_in_pool = 0;
    return AEVK_OK;
}

AevkMaterial* aevk_material_create(AevkPipeline* p) {
    aevk_clear_error();
    if (!p) { aevk_fail(AEVK_ERR_ARG, "pipeline is null"); return NULL; }
    if (!p->set_layout) {
        aevk_fail(AEVK_ERR_ARG,
                  "pipeline was created without bindings, so it has no materials");
        return NULL;
    }
    AevkDevice* d = p->dev;

    if (p->pool_count == 0 || p->sets_in_pool >= AEVK_SETS_PER_POOL) {
        if (aevk_pipeline_add_pool(p) != AEVK_OK) return NULL;
    }

    AevkMaterial* m = (AevkMaterial*)calloc(1, sizeof(*m));
    if (!m) { aevk_fail(AEVK_ERR_OOM, "out of memory"); return NULL; }
    m->pipe = p;

    VkDescriptorSetAllocateInfo dsi = {0};
    dsi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsi.descriptorPool = p->pools[p->pool_count - 1];
    dsi.descriptorSetCount = 1;
    dsi.pSetLayouts = &p->set_layout;
    VkResult r = d->da.vkAllocateDescriptorSets(d->device, &dsi, &m->set);

    /* A driver may report the pool full before maxSets is reached, since
     * descriptor counts can run out first. Take that as "add a pool" rather
     * than as failure. */
    if (r == VK_ERROR_OUT_OF_POOL_MEMORY || r == VK_ERROR_FRAGMENTED_POOL) {
        if (aevk_pipeline_add_pool(p) != AEVK_OK) { free(m); return NULL; }
        dsi.descriptorPool = p->pools[p->pool_count - 1];
        r = d->da.vkAllocateDescriptorSets(d->device, &dsi, &m->set);
    }
    if (r != VK_SUCCESS) {
        aevk_fail(AEVK_ERR_OOM, "vkAllocateDescriptorSets failed (%d)", (int)r);
        free(m);
        return NULL;
    }
    p->sets_in_pool++;
    return m;
}

/* The set itself is owned by its pool and goes when the pipeline does; what
 * this reclaims is the uniform buffers written into it. Destroy materials
 * before the pipeline that made them. */
void aevk_material_destroy(AevkMaterial* m) {
    if (!m) return;
    AevkPipeline* p = m->pipe;
    if (p && p->dev && p->dev->device) {
        AevkDevice* d = p->dev;
        for (int i = 0; i < AEVK_MAX_DESC; i++) {
            if (m->ub[i].ptr) d->da.vkUnmapMemory(d->device, m->ub[i].mem);
            if (m->ub[i].buf) d->da.vkDestroyBuffer(d->device, m->ub[i].buf, NULL);
            if (m->ub[i].mem) d->da.vkFreeMemory(d->device, m->ub[i].mem, NULL);
        }
    }
    free(m);
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
    /* A target feeds one vertex stream, binding 0 (verts_reserve fills it).
     * A layout declaring another would have the pipeline read a buffer that
     * is never bound, so it is refused here rather than drawn from. */
    for (uint32_t i = 0; layout && i < layout->bind_count; i++) {
        if (layout->binds[i].binding != 0) {
            aevk_fail(AEVK_ERR_ARG, "vertex binding %u is declared, but a target feeds binding 0 only",
                      layout->binds[i].binding);
            return NULL;
        }
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

        VkDescriptorPoolSize sizes[3] = {{0}, {0}, {0}};
        uint32_t nsizes = 0, n_ub = 0, n_img = 0, n_sb = 0;
        for (uint32_t i = 0; i < bindings->count; i++) {
            VkDescriptorType ty = bindings->b[i].descriptorType;
            if (ty == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) n_ub++;
            else if (ty == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) n_sb++;
            else n_img++;
            p->declared[bindings->b[i].binding] = 1;
            p->desc_type[bindings->b[i].binding] = ty;
        }
        if (n_ub)  { sizes[nsizes].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                     sizes[nsizes++].descriptorCount = n_ub; }
        if (n_img) { sizes[nsizes].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                     sizes[nsizes++].descriptorCount = n_img; }
        if (n_sb)  { sizes[nsizes].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                     sizes[nsizes++].descriptorCount = n_sb; }

        /* Counts are per set; a pool holds AEVK_SETS_PER_POOL of them. */
        for (uint32_t i = 0; i < nsizes; i++) {
            p->pool_sizes[i] = sizes[i];
            p->pool_sizes[i].descriptorCount = sizes[i].descriptorCount * AEVK_SETS_PER_POOL;
        }
        p->pool_size_count = nsizes;

        p->def = aevk_material_create(p);
        if (!p->def) goto fail;
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

    /* A layout with nothing described in it is a pipeline with no vertex
     * input at all: the vertex shader pulls its data from a storage buffer
     * by gl_VertexIndex (#1515), and the draw still takes its count from the
     * target's reserved vertices. */
    VkPipelineVertexInputStateCreateInfo vin = {0};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    p->vertex_input = !layout || layout->bind_count > 0;
    if (layout) {
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
    /* Must match the render pass's colour attachment, so it comes from the
     * target rather than being fixed at one sample. */
    ms.rasterizationSamples = aevk_sample_bit(t->samples ? t->samples : 1);

    VkPipelineDepthStencilStateCreateInfo ds = {0};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = t->has_depth ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = t->has_depth ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    ds.minDepthBounds = 0.0f;
    ds.maxDepthBounds = 1.0f;

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
    if (t->has_depth) gpi.pDepthStencilState = &ds;
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
        AEVK_MUTEX_LOCK(&d->lock);
        d->da.vkDeviceWaitIdle(d->device);
        if (p->pipeline) d->da.vkDestroyPipeline(d->device, p->pipeline, NULL);
        if (p->layout)   d->da.vkDestroyPipelineLayout(d->device, p->layout, NULL);
        if (p->frag)     d->da.vkDestroyShaderModule(d->device, p->frag, NULL);
        if (p->vert)     d->da.vkDestroyShaderModule(d->device, p->vert, NULL);
        /* Sets are freed with their pool; the layout outlives neither. */
        aevk_material_destroy(p->def);
        p->def = NULL;
        for (int i = 0; i < p->pool_count; i++) {
            d->da.vkDestroyDescriptorPool(d->device, p->pools[i], NULL);
        }
        if (p->set_layout) d->da.vkDestroyDescriptorSetLayout(d->device, p->set_layout, NULL);
        AEVK_MUTEX_UNLOCK(&d->lock);
    }
    free(p);
}

/* ------------------------------------------------------------------------ */
/* Shader resources: uniform buffers, textures, push constants              */
/* ------------------------------------------------------------------------ */

int aevk_material_set_uniform(AevkMaterial* m, int binding,
                              const void* data, size_t len) {
    aevk_clear_error();
    if (!m) return aevk_fail(AEVK_ERR_ARG, "material is null");
    AevkPipeline* p = m->pipe;
    if (!p || !data) return aevk_fail(AEVK_ERR_ARG, "pipeline or data is null");
    if (binding < 0 || binding >= AEVK_MAX_DESC) {
        return aevk_fail(AEVK_ERR_ARG, "binding must be 0..%d", AEVK_MAX_DESC - 1);
    }
    if (len == 0) return aevk_fail(AEVK_ERR_ARG, "uniform data is empty");
    if (!m->set) {
        return aevk_fail(AEVK_ERR_ARG,
                         "pipeline was created without bindings, so it has no descriptor set");
    }
    if (!p->declared[binding] || p->desc_type[binding] != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
        return aevk_fail(AEVK_ERR_ARG, "binding %d is not declared as a uniform", binding);
    }
    AevkDevice* d = p->dev;

    /* Reuse the buffer while it is big enough; a uniform that changes every
     * frame must not allocate every frame. */
    if (m->ub[binding].buf && m->ub[binding].size < (VkDeviceSize)len) {
        if (m->ub[binding].ptr) d->da.vkUnmapMemory(d->device, m->ub[binding].mem);
        d->da.vkDestroyBuffer(d->device, m->ub[binding].buf, NULL);
        d->da.vkFreeMemory(d->device, m->ub[binding].mem, NULL);
        m->ub[binding].buf = VK_NULL_HANDLE;
        m->ub[binding].mem = VK_NULL_HANDLE;
        m->ub[binding].ptr = NULL;
        m->ub[binding].size = 0;
    }

    /* A caller's buffer was bound here: the uniform goes back to being the
     * material's own, so the descriptor is rewritten below. */
    int repoint = m->ext[binding] != NULL && m->ub[binding].buf != VK_NULL_HANDLE;
    m->ext[binding] = NULL;

    if (!m->ub[binding].buf) {
        int rc = aevk_make_buffer(d, (VkDeviceSize)len, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  &m->ub[binding].buf, &m->ub[binding].mem);
        if (rc != AEVK_OK) return rc;
        VkResult r = d->da.vkMapMemory(d->device, m->ub[binding].mem, 0,
                                       (VkDeviceSize)len, 0, &m->ub[binding].ptr);
        if (r != VK_SUCCESS) {
            d->da.vkDestroyBuffer(d->device, m->ub[binding].buf, NULL);
            d->da.vkFreeMemory(d->device, m->ub[binding].mem, NULL);
            m->ub[binding].buf = VK_NULL_HANDLE;
            m->ub[binding].mem = VK_NULL_HANDLE;
            return aevk_fail(AEVK_ERR_OOM, "vkMapMemory failed (%d)", (int)r);
        }
        m->ub[binding].size = (VkDeviceSize)len;

        VkDescriptorBufferInfo bi = {0};
        bi.buffer = m->ub[binding].buf;
        bi.range = (VkDeviceSize)len;
        VkWriteDescriptorSet w = {0};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = m->set;
        w.dstBinding = (uint32_t)binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &bi;
        d->da.vkUpdateDescriptorSets(d->device, 1, &w, 0, NULL);
        m->writes++;
    } else if (repoint) {
        VkDescriptorBufferInfo bi = {0};
        bi.buffer = m->ub[binding].buf;
        bi.range = m->ub[binding].size;
        VkWriteDescriptorSet w = {0};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = m->set;
        w.dstBinding = (uint32_t)binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &bi;
        d->da.vkUpdateDescriptorSets(d->device, 1, &w, 0, NULL);
    }

    /* Host-coherent, so the write is visible without a flush. The descriptor
     * already points at this buffer, so no set update is needed per frame. */
    memcpy(m->ub[binding].ptr, data, len);
    return AEVK_OK;
}

int aevk_material_set_texture(AevkMaterial* m, int binding, AevkTexture* tex) {
    aevk_clear_error();
    if (!m) return aevk_fail(AEVK_ERR_ARG, "material is null");
    AevkPipeline* p = m->pipe;
    if (!p || !tex) return aevk_fail(AEVK_ERR_ARG, "pipeline or texture is null");
    if (binding < 0 || binding >= AEVK_MAX_DESC) {
        return aevk_fail(AEVK_ERR_ARG, "binding must be 0..%d", AEVK_MAX_DESC - 1);
    }
    if (!m->set) {
        return aevk_fail(AEVK_ERR_ARG,
                         "pipeline was created without bindings, so it has no descriptor set");
    }
    if (!tex->uploaded) {
        /* Sampling an image still in UNDEFINED layout is undefined behaviour
         * and reads as garbage, so refuse rather than render nonsense. */
        return aevk_fail(AEVK_ERR_ARG, "texture has no pixels yet, upload before binding");
    }
    if (!p->declared[binding] ||
        p->desc_type[binding] != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
        return aevk_fail(AEVK_ERR_ARG, "binding %d is not declared as a texture", binding);
    }
    AevkDevice* d = p->dev;

    VkDescriptorImageInfo ii = {0};
    ii.sampler = tex->sampler;
    ii.imageView = tex->view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w = {0};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = m->set;
    w.dstBinding = (uint32_t)binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    d->da.vkUpdateDescriptorSets(d->device, 1, &w, 0, NULL);
    m->writes++;
    return AEVK_OK;
}
/* The pipeline-level writers address its default material, so callers that
 * never ask for one see no change. */
int aevk_pipeline_set_uniform(AevkPipeline* p, int binding, const void* data, size_t len) {
    aevk_clear_error();
    if (!p) return aevk_fail(AEVK_ERR_ARG, "pipeline is null");
    if (!p->def) {
        return aevk_fail(AEVK_ERR_ARG,
                         "pipeline was created without bindings, so it has no descriptor set");
    }
    return aevk_material_set_uniform(p->def, binding, data, len);
}

int aevk_pipeline_set_texture(AevkPipeline* p, int binding, AevkTexture* tex) {
    aevk_clear_error();
    if (!p) return aevk_fail(AEVK_ERR_ARG, "pipeline is null");
    if (!p->def) {
        return aevk_fail(AEVK_ERR_ARG,
                         "pipeline was created without bindings, so it has no descriptor set");
    }
    return aevk_material_set_texture(p->def, binding, tex);
}

/* Points a storage or uniform binding at a caller's buffer (#1515): a vertex
 * or fragment shader reading what a compute pass wrote. The buffer must
 * outlive every draw that uses the material. */
int aevk_material_set_buffer(AevkMaterial* m, int binding, AevkBuffer* buf) {
    aevk_clear_error();
    if (!m || !buf) return aevk_fail(AEVK_ERR_ARG, "material or buffer is null");
    AevkPipeline* p = m->pipe;
    if (binding < 0 || binding >= AEVK_MAX_DESC) {
        return aevk_fail(AEVK_ERR_ARG, "binding must be 0..%d", AEVK_MAX_DESC - 1);
    }
    if (!m->set) {
        return aevk_fail(AEVK_ERR_ARG,
                         "pipeline was created without bindings, so it has no descriptor set");
    }
    if (buf->dev != p->dev) return aevk_fail(AEVK_ERR_ARG, "the buffer belongs to another device");
    VkDescriptorType ty = p->desc_type[binding];
    if (!p->declared[binding] ||
        (ty != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && ty != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)) {
        return aevk_fail(AEVK_ERR_ARG, "binding %d is not declared as a storage or uniform buffer",
                         binding);
    }
    VkDescriptorBufferInfo bi = {0};
    bi.buffer = buf->buf;
    bi.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet w = {0};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = m->set;
    w.dstBinding = (uint32_t)binding;
    w.descriptorCount = 1;
    w.descriptorType = ty;
    w.pBufferInfo = &bi;
    p->dev->da.vkUpdateDescriptorSets(p->dev->device, 1, &w, 0, NULL);
    m->ext[binding] = buf;
    m->writes++;
    return AEVK_OK;
}

int aevk_pipeline_set_buffer(AevkPipeline* p, int binding, AevkBuffer* buf) {
    aevk_clear_error();
    if (!p) return aevk_fail(AEVK_ERR_ARG, "pipeline is null");
    if (!p->def) {
        return aevk_fail(AEVK_ERR_ARG,
                         "pipeline was created without bindings, so it has no descriptor set");
    }
    return aevk_material_set_buffer(p->def, binding, buf);
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
int aevk_ae_indices_reserve_ex(void* tp, int count, int bits);

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

static int aevk_record(AevkTarget* t, AevkFrame* fr, AevkPipeline* p, AevkMaterial* mat,
                       float r, float g, float b, float a) {
    AevkDevice* d = t->dev;
    VkResult vr = d->da.vkResetCommandBuffer(fr->cmd, 0);
    if (vr != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkResetCommandBuffer failed (%d)", (int)vr);

    VkCommandBufferBeginInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vr = d->da.vkBeginCommandBuffer(fr->cmd, &bi);
    if (vr != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkBeginCommandBuffer failed (%d)", (int)vr);

    /* Indexed by attachment, so the resolve slot is present but unused and the
     * depth slot sits wherever the render pass put it. Cleared to the far
     * plane: with a LESS test, anything drawn is nearer than nothing. */
    VkClearValue clears[3];
    memset(clears, 0, sizeof(clears));
    clears[0].color.float32[0] = r;
    clears[0].color.float32[1] = g;
    clears[0].color.float32[2] = b;
    clears[0].color.float32[3] = a;
    if (t->has_depth) {
        clears[t->depth_clear_index].depthStencil.depth = 1.0f;
        clears[t->depth_clear_index].depthStencil.stencil = 0;
    }

    VkRenderPassBeginInfo rp = {0};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = t->pass;
    rp.framebuffer = t->fb;
    rp.renderArea.extent.width = (uint32_t)t->width;
    rp.renderArea.extent.height = (uint32_t)t->height;
    rp.clearValueCount = t->clear_count ? t->clear_count : 1;
    rp.pClearValues = clears;

    d->da.vkCmdBeginRenderPass(fr->cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    if (p && t->vertex_count > 0) {
        VkViewport vp = {0};
        vp.width = (float)t->width;
        vp.height = (float)t->height;
        vp.maxDepth = 1.0f;
        VkRect2D sc = {{0, 0}, {(uint32_t)t->width, (uint32_t)t->height}};
        d->da.vkCmdSetViewport(fr->cmd, 0, 1, &vp);
        d->da.vkCmdSetScissor(fr->cmd, 0, 1, &sc);
        d->da.vkCmdBindPipeline(fr->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
        /* The material decides which set is bound; without one the pipeline's
         * default is used, which is what a caller that never asked for
         * materials has. */
        AevkMaterial* bind_mat = mat ? mat : p->def;
        /* `writes`, not just a non-null handle: a set fresh out of the pool has
         * no descriptors, and lavapipe walks a set as it is bound. */
        if (t->batch_count == 0 && bind_mat && bind_mat->set && bind_mat->writes > 0) {
            d->da.vkCmdBindDescriptorSets(fr->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          p->layout, 0, 1, &bind_mat->set, 0, NULL);
        }
        /* Push whatever the pipeline declared room for, so a caller who set
         * fewer bytes than the range still gets a defined block. */
        if (p->push_bytes > 0) {
            unsigned char block[AEVK_MAX_PUSH];
            memset(block, 0, sizeof(block));
            uint32_t n = t->push_size < p->push_bytes ? t->push_size : p->push_bytes;
            if (n) memcpy(block, t->push_data, n);
            d->da.vkCmdPushConstants(fr->cmd, p->layout,
                                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                     0, p->push_bytes, block);
        }
        if (p->vertex_input) {
            VkDeviceSize off = 0;
            d->da.vkCmdBindVertexBuffers(fr->cmd, 0, 1, &t->vbuf, &off);
        }
        if (t->index_count > 0) {
            d->da.vkCmdBindIndexBuffer(fr->cmd, t->ibuf, 0,
                                       (t->index_bits == 16) ? VK_INDEX_TYPE_UINT16
                                                             : VK_INDEX_TYPE_UINT32);
        }
        if (t->batch_count > 0) {
            /* Several draws inside the one render pass, each free to bind its
             * own material. Rebinding the set between draws is the whole
             * reason per-draw sets exist: with one set per pipeline the second
             * draw would overwrite what the first is still going to read. */
            for (int i = 0; i < t->batch_count; i++) {
                AevkDrawItem* it = &t->batch[i];
                AevkMaterial* im = it->mat ? it->mat : bind_mat;
                if (im && im->set && im->writes > 0) {
                    d->da.vkCmdBindDescriptorSets(fr->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                  p->layout, 0, 1, &im->set, 0, NULL);
                }
                if (t->index_count > 0) {
                    d->da.vkCmdDrawIndexed(fr->cmd, (uint32_t)it->count, 1,
                                           (uint32_t)it->first, 0, 0);
                } else {
                    d->da.vkCmdDraw(fr->cmd, (uint32_t)it->count, 1, (uint32_t)it->first, 0);
                }
            }
        } else if (t->index_count > 0) {
            d->da.vkCmdDrawIndexed(fr->cmd, (uint32_t)t->index_count, 1, 0, 0, 0);
        } else {
            d->da.vkCmdDraw(fr->cmd, (uint32_t)t->vertex_count, 1, 0, 0);
        }
    }
    d->da.vkCmdEndRenderPass(fr->cmd);

    if (t->readback_on) {
        VkBufferImageCopy copy = {0};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = (uint32_t)t->width;
        copy.imageExtent.height = (uint32_t)t->height;
        copy.imageExtent.depth = 1;
        d->da.vkCmdCopyImageToBuffer(fr->cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     fr->readback, 1, &copy);
    }

    vr = d->da.vkEndCommandBuffer(fr->cmd);
    if (vr != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkEndCommandBuffer failed (%d)", (int)vr);

    fr->recorded = 1;
    fr->rec_pipe = p;
    fr->rec_mat = mat;
    fr->rec_vertices = t->vertex_count;
    fr->rec_clear[0] = r; fr->rec_clear[1] = g; fr->rec_clear[2] = b; fr->rec_clear[3] = a;
    fr->rec_indices = t->index_count;
    fr->rec_push_size = t->push_size;
    if (t->push_size) memcpy(fr->rec_push, t->push_data, t->push_size);
    fr->rec_batch_version = t->batch_version;
    fr->rec_batch_count = t->batch_count;
    return AEVK_OK;
}

static int aevk_draw_locked(AevkTarget* t, AevkPipeline* p, AevkMaterial* mat,
                            float r, float g, float b, float a);

int aevk_draw(AevkTarget* t, AevkPipeline* p, float r, float g, float b, float a) {
    aevk_clear_error();
    if (!t) return aevk_fail(AEVK_ERR_ARG, "target is null");
    if (p && p->dev != t->dev) return aevk_fail(AEVK_ERR_ARG, "pipeline belongs to another device");
    if (p && t->vertex_count > 0 && !t->vbuf) {
        return aevk_fail(AEVK_ERR_ARG, "vertices were never uploaded");
    }

    AevkDevice* d = t->dev;
    AEVK_MUTEX_LOCK(&d->lock);
    int rc = aevk_draw_locked(t, p, NULL, r, g, b, a);
    AEVK_MUTEX_UNLOCK(&d->lock);
    return rc;
}

/* The device lock is held. Records into the target's command buffer, which
 * came from the device's pool, then submits to the device's queue: both need
 * the caller to synchronise, which is what the lock in aevk_draw is for. */
static const unsigned char* aevk_readable_pixels(AevkTarget* t);

/* Geometry and push data are per TARGET, so a change invalidates the recorded
 * commands in every slot, not just the next one. Missing a slot would redraw
 * an old frame `frame_count` submissions later, which is exactly the kind of
 * bug that only shows up once someone turns pipelining on. */
static void aevk_invalidate_records(AevkTarget* t) {
    for (int i = 0; i < t->frame_count; i++) t->frames[i].recorded = 0;
}

/* Waits for one slot's work, if any is outstanding, and marks it readable.
 * The device lock is held. */
static int aevk_wait_frame_locked(AevkTarget* t, int slot) {
    AevkDevice* d = t->dev;
    AevkFrame* fr = &t->frames[slot];
    if (!fr->submitted) return AEVK_OK;

    VkResult vr = d->da.vkWaitForFences(d->device, 1, &fr->fence, VK_TRUE, t->timeout_ns);
    if (vr == VK_TIMEOUT) {
        return aevk_fail(AEVK_ERR_DEVICE_LOST, "GPU did not finish within %llu ms",
                         (unsigned long long)(t->timeout_ns / 1000000ull));
    }
    if (vr != VK_SUCCESS) {
        return aevk_fail(vr == VK_ERROR_DEVICE_LOST ? AEVK_ERR_DEVICE_LOST : AEVK_ERR_OOM,
                         "vkWaitForFences failed (%d)", (int)vr);
    }
    fr->submitted = 0;
    t->last_done = slot;
    return AEVK_OK;
}

/* Records into the next slot and hands it to the queue WITHOUT waiting. The
 * device lock is held. Returns the slot, or a negative status.
 *
 * The slot is reused round-robin, so the wait here is for the work this slot
 * held `frame_count` submissions ago, not for the one just queued: that is the
 * whole point, and it is also what bounds the queue depth. */
static int aevk_submit_locked(AevkTarget* t, AevkPipeline* p, AevkMaterial* mat,
                              float r, float g, float b, float a) {
    AevkDevice* d = t->dev;
    int slot = t->next_frame;

    if (!t->fb || !t->frames) {
        return aevk_fail(AEVK_ERR_ARG, "target has no images: its last resize failed");
    }
    if (t->batch_count > 0 && p) {
        int brc = aevk_batch_check(t, p);
        if (brc != AEVK_OK) return brc;
    }

    int rc = aevk_wait_frame_locked(t, slot);
    if (rc != AEVK_OK) return rc;

    AevkFrame* fr = &t->frames[slot];
    int stale = !fr->recorded || fr->rec_pipe != p || fr->rec_mat != mat ||
                fr->rec_vertices != t->vertex_count ||
                fr->rec_clear[0] != r || fr->rec_clear[1] != g ||
                fr->rec_clear[2] != b || fr->rec_clear[3] != a ||
                fr->rec_indices != t->index_count ||
                fr->rec_push_size != t->push_size ||
                fr->rec_batch_version != t->batch_version ||
                fr->rec_batch_count != t->batch_count ||
                (t->push_size && memcmp(fr->rec_push, t->push_data, t->push_size) != 0);
    if (stale) {
        rc = aevk_record(t, fr, p, mat, r, g, b, a);
        if (rc != AEVK_OK) { fr->recorded = 0; return rc; }
    }

    VkResult vr = d->da.vkResetFences(d->device, 1, &fr->fence);
    if (vr != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkResetFences failed (%d)", (int)vr);

    VkSubmitInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &fr->cmd;
    vr = d->da.vkQueueSubmit(d->queue, 1, &si, fr->fence);
    if (vr != VK_SUCCESS) {
        return aevk_fail(vr == VK_ERROR_DEVICE_LOST ? AEVK_ERR_DEVICE_LOST : AEVK_ERR_OOM,
                         "vkQueueSubmit failed (%d)", (int)vr);
    }
    fr->submitted = 1;
    t->rendered = 1;
    t->last_submitted = slot;
    t->next_frame = (slot + 1) % t->frame_count;
    return slot;
}

static int aevk_draw_locked(AevkTarget* t, AevkPipeline* p, AevkMaterial* mat,
                            float r, float g, float b, float a) {
    int slot = aevk_submit_locked(t, p, mat, r, g, b, a);
    if (slot < 0) return slot;
    return aevk_wait_frame_locked(t, slot);
}

/* Frames in flight. 1 is the synchronous shape and the default: draw()
 * records, submits and waits, which is what makes the phase 1 test
 * deterministic. Above 1 the caller can submit() several frames before
 * waiting, and the CPU records frame N+1 while the GPU runs frame N.
 *
 * Each slot costs its own readback buffer, width*height*4, which is why this
 * is opt-in rather than a default of 2 or 3. */
int aevk_target_set_frames(AevkTarget* t, int count) {
    aevk_clear_error();
    if (!t) return aevk_fail(AEVK_ERR_ARG, "target is null");
    if (count < 1 || count > 8) {
        return aevk_fail(AEVK_ERR_ARG, "frames in flight must be 1..8 (got %d)", count);
    }
    if (count == t->frame_count) return AEVK_OK;

    AevkDevice* d = t->dev;
    AEVK_MUTEX_LOCK(&d->lock);
    d->da.vkDeviceWaitIdle(d->device);
    aevk_frames_free(t);
    int rc = aevk_frames_alloc(t, count);
    AEVK_MUTEX_UNLOCK(&d->lock);
    return rc;
}

int aevk_target_frames(const AevkTarget* t) { return t ? t->frame_count : 0; }

/* The fence wait, in milliseconds. The default of five seconds is a hang
 * detector rather than a frame budget: a correct driver finishes offscreen
 * work in microseconds, so anything near it means the device is wedged.
 * A caller rendering something genuinely heavy can raise it, and one that
 * would rather fail fast can lower it. */
int aevk_target_set_timeout_ms(AevkTarget* t, int ms) {
    aevk_clear_error();
    if (!t) return aevk_fail(AEVK_ERR_ARG, "target is null");
    if (ms <= 0) return aevk_fail(AEVK_ERR_ARG, "timeout must be positive (got %d)", ms);
    t->timeout_ns = (uint64_t)ms * 1000000ull;
    return AEVK_OK;
}

/* Records and submits without waiting. Returns the slot it used, or a
 * negative status. Blocks only when every slot is still in flight, which is
 * what bounds the queue depth to frame_count. */
int aevk_submit(AevkTarget* t, AevkPipeline* p, float r, float g, float b, float a) {
    aevk_clear_error();
    if (!t) return aevk_fail(AEVK_ERR_ARG, "target is null");
    if (p && p->dev != t->dev) return aevk_fail(AEVK_ERR_ARG, "pipeline belongs to another device");
    if (p && t->vertex_count > 0 && !t->vbuf) {
        return aevk_fail(AEVK_ERR_ARG, "vertices were never uploaded");
    }
    AevkDevice* d = t->dev;
    AEVK_MUTEX_LOCK(&d->lock);
    int slot = aevk_submit_locked(t, p, NULL, r, g, b, a);
    AEVK_MUTEX_UNLOCK(&d->lock);
    return slot;
}

/* Draws with a specific material, so one pipeline serves several objects with
 * different textures and constants in a frame. Otherwise identical to draw. */
int aevk_draw_material(AevkTarget* t, AevkPipeline* p, AevkMaterial* mat,
                       float r, float g, float b, float a) {
    aevk_clear_error();
    if (!t) return aevk_fail(AEVK_ERR_ARG, "target is null");
    if (p && p->dev != t->dev) return aevk_fail(AEVK_ERR_ARG, "pipeline belongs to another device");
    if (mat && mat->pipe != p) return aevk_fail(AEVK_ERR_ARG, "material belongs to another pipeline");
    if (p && t->vertex_count > 0 && !t->vbuf) {
        return aevk_fail(AEVK_ERR_ARG, "vertices were never uploaded");
    }
    AevkDevice* d = t->dev;
    AEVK_MUTEX_LOCK(&d->lock);
    int rc = aevk_draw_locked(t, p, mat, r, g, b, a);
    AEVK_MUTEX_UNLOCK(&d->lock);
    return rc;
}

/* The non-blocking sibling of draw_material. */
int aevk_submit_material(AevkTarget* t, AevkPipeline* p, AevkMaterial* mat,
                         float r, float g, float b, float a) {
    aevk_clear_error();
    if (!t) return aevk_fail(AEVK_ERR_ARG, "target is null");
    if (p && p->dev != t->dev) return aevk_fail(AEVK_ERR_ARG, "pipeline belongs to another device");
    if (mat && mat->pipe != p) return aevk_fail(AEVK_ERR_ARG, "material belongs to another pipeline");
    AevkDevice* d = t->dev;
    AEVK_MUTEX_LOCK(&d->lock);
    int slot = aevk_submit_locked(t, p, mat, r, g, b, a);
    AEVK_MUTEX_UNLOCK(&d->lock);
    return slot;
}

/* Waits for everything outstanding. After this every submitted frame has
 * completed and the most recent one is what the readers see. */
int aevk_wait_all(AevkTarget* t) {
    aevk_clear_error();
    if (!t) return aevk_fail(AEVK_ERR_ARG, "target is null");
    AevkDevice* d = t->dev;
    AEVK_MUTEX_LOCK(&d->lock);
    int rc = AEVK_OK;
    /* Oldest first, so last_done ends on the most recently submitted slot
     * rather than on whichever happened to be waited on last. */
    for (int i = 0; i < t->frame_count; i++) {
        int slot = (t->next_frame + i) % t->frame_count;
        int one = aevk_wait_frame_locked(t, slot);
        if (one != AEVK_OK) rc = one;
    }
    AEVK_MUTEX_UNLOCK(&d->lock);
    return rc;
}

/* Every pixel as 8-bit RGBA, whatever the target's format: width*height*4
 * bytes, float channels clamped to 0..1. What the image writers use. */
int aevk_read_rgba8(AevkTarget* t, void* out, size_t out_len) {
    aevk_clear_error();
    if (!t || !out) return aevk_fail(AEVK_ERR_ARG, "target or destination is null");
    if (!t->readback_on) {
        return aevk_fail(AEVK_ERR_ARG, "readback is off for this target (target_set_readback)");
    }
    size_t pixels = (size_t)t->width * (size_t)t->height;
    if (out_len < pixels * 4u) {
        return aevk_fail(AEVK_ERR_ARG, "destination holds %zu bytes, the image needs %zu",
                         out_len, pixels * 4u);
    }
    const unsigned char* src = aevk_readable_pixels(t);
    if (!src) return aevk_fail(AEVK_ERR_ARG, "target has no readback mapping");
    unsigned char* dst = (unsigned char*)out;
    if (t->bytes_per_pixel == 4) {
        memcpy(dst, src, pixels * 4u);
        return AEVK_OK;
    }
    for (size_t i = 0; i < pixels; i++) {
        const unsigned char* px = src + i * (size_t)t->bytes_per_pixel;
        for (int c = 0; c < 4; c++) dst[i * 4u + (size_t)c] = aevk_channel_u8(t, px, c);
    }
    return AEVK_OK;
}

int aevk_read_rgba(AevkTarget* t, void* out, size_t out_len) {
    aevk_clear_error();
    if (!t || !out) return aevk_fail(AEVK_ERR_ARG, "target or destination is null");
    if (!t->readback_on) {
        return aevk_fail(AEVK_ERR_ARG, "readback is off for this target (target_set_readback)");
    }
    if (out_len < (size_t)t->readback_size) {
        return aevk_fail(AEVK_ERR_ARG, "destination holds %zu bytes, the image needs %zu",
                         out_len, (size_t)t->readback_size);
    }
    if (!t->frames) return aevk_fail(AEVK_ERR_ARG, "target has no readback mapping");

    /* Read the slot that finished most recently, waiting for it first: with
     * frames in flight the caller may not have waited, and reading a buffer
     * the GPU is still writing would hand back a torn frame. */
    const unsigned char* src = aevk_readable_pixels(t);
    if (!src) return aevk_fail(AEVK_ERR_ARG, "target has no readback mapping");
    memcpy(out, src, (size_t)t->readback_size);
    return AEVK_OK;
}

/* ------------------------------------------------------------------------ */
/* Buffers and compute (#1515)                                               */
/* ------------------------------------------------------------------------ */

AevkBuffer* aevk_buffer_create(AevkDevice* d, size_t bytes) {
    aevk_clear_error();
    if (!d) { aevk_fail(AEVK_ERR_ARG, "device is null"); return NULL; }
    if (bytes == 0) { aevk_fail(AEVK_ERR_ARG, "a buffer needs at least one byte"); return NULL; }
    AevkBuffer* b = (AevkBuffer*)calloc(1, sizeof(*b));
    if (!b) { aevk_fail(AEVK_ERR_OOM, "out of memory"); return NULL; }
    b->dev = d;
    b->size = (VkDeviceSize)bytes;
    /* Every use this module has for a buffer: read and written by a compute
     * pass, read as a uniform, pulled from by a vertex shader, and copied. */
    int rc = aevk_make_buffer(d, (VkDeviceSize)bytes,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              &b->buf, &b->mem);
    if (rc != AEVK_OK) { free(b); return NULL; }
    VkResult r = d->da.vkMapMemory(d->device, b->mem, 0, b->size, 0, &b->ptr);
    if (r != VK_SUCCESS) {
        aevk_fail(AEVK_ERR_OOM, "vkMapMemory failed (%d)", (int)r);
        aevk_buffer_destroy(b);
        return NULL;
    }
    /* Zeroed, so a shader reading a buffer the program never filled reads
     * zeros rather than whatever the allocation last held. */
    memset(b->ptr, 0, bytes);
    return b;
}

void aevk_buffer_destroy(AevkBuffer* b) {
    if (!b) return;
    AevkDevice* d = b->dev;
    if (d && d->device) {
        /* The GPU may still be reading it from a submitted frame or dispatch. */
        AEVK_MUTEX_LOCK(&d->lock);
        d->da.vkDeviceWaitIdle(d->device);
        if (b->ptr) d->da.vkUnmapMemory(d->device, b->mem);
        if (b->buf) d->da.vkDestroyBuffer(d->device, b->buf, NULL);
        if (b->mem) d->da.vkFreeMemory(d->device, b->mem, NULL);
        AEVK_MUTEX_UNLOCK(&d->lock);
    }
    free(b);
}

size_t aevk_buffer_size(const AevkBuffer* b) { return b ? (size_t)b->size : 0; }

int aevk_buffer_write(AevkBuffer* b, size_t offset, const void* data, size_t len) {
    aevk_clear_error();
    if (!b || !data) return aevk_fail(AEVK_ERR_ARG, "buffer or data is null");
    if (offset > (size_t)b->size || len > (size_t)b->size - offset) {
        return aevk_fail(AEVK_ERR_ARG, "writing %zu bytes at %zu overruns a %zu-byte buffer",
                         len, offset, (size_t)b->size);
    }
    memcpy((char*)b->ptr + offset, data, len);
    return AEVK_OK;
}

int aevk_buffer_read(AevkBuffer* b, size_t offset, void* out, size_t len) {
    aevk_clear_error();
    if (!b || !out) return aevk_fail(AEVK_ERR_ARG, "buffer or destination is null");
    if (offset > (size_t)b->size || len > (size_t)b->size - offset) {
        return aevk_fail(AEVK_ERR_ARG, "reading %zu bytes at %zu overruns a %zu-byte buffer",
                         len, offset, (size_t)b->size);
    }
    memcpy(out, (const char*)b->ptr + offset, len);
    return AEVK_OK;
}

struct AevkCompute {
    AevkDevice*           dev;
    VkShaderModule        module;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool      pool;
    VkDescriptorSet       set;
    VkPipelineLayout      layout;
    VkPipeline            pipeline;
    uint32_t              push_bytes;
    unsigned char         push[AEVK_MAX_PUSH];
    int                   declared[AEVK_MAX_DESC];
    VkDescriptorType      desc_type[AEVK_MAX_DESC];
    int                   written[AEVK_MAX_DESC];
    VkCommandBuffer       cmd;
    VkFence               fence;
    int                   submitted;
    uint64_t              timeout_ns;
};

void aevk_compute_destroy(AevkCompute* c) {
    if (!c) return;
    AevkDevice* d = c->dev;
    if (d && d->device) {
        AEVK_MUTEX_LOCK(&d->lock);
        d->da.vkDeviceWaitIdle(d->device);
        if (c->fence)      d->da.vkDestroyFence(d->device, c->fence, NULL);
        if (c->cmd)        d->da.vkFreeCommandBuffers(d->device, d->pool, 1, &c->cmd);
        if (c->pipeline)   d->da.vkDestroyPipeline(d->device, c->pipeline, NULL);
        if (c->layout)     d->da.vkDestroyPipelineLayout(d->device, c->layout, NULL);
        if (c->pool)       d->da.vkDestroyDescriptorPool(d->device, c->pool, NULL);
        if (c->set_layout) d->da.vkDestroyDescriptorSetLayout(d->device, c->set_layout, NULL);
        if (c->module)     d->da.vkDestroyShaderModule(d->device, c->module, NULL);
        AEVK_MUTEX_UNLOCK(&d->lock);
    }
    free(c);
}

AevkCompute* aevk_compute_create(AevkDevice* d, const void* spv, size_t len,
                                 const AevkBindings* bindings, int push_bytes) {
    aevk_clear_error();
    if (!d) { aevk_fail(AEVK_ERR_ARG, "device is null"); return NULL; }
    if (!spv) { aevk_fail(AEVK_ERR_ARG, "shader bytes are null"); return NULL; }
    if (len == 0 || (len % 4)) {
        aevk_fail(AEVK_ERR_SHADER, "SPIR-V length must be a non-zero multiple of 4 (got %zu)", len);
        return NULL;
    }
    if (push_bytes < 0 || push_bytes > AEVK_MAX_PUSH || (push_bytes % 4)) {
        aevk_fail(AEVK_ERR_ARG,
                  "push constant block must be 0..%d bytes and a multiple of 4 (got %d)",
                  AEVK_MAX_PUSH, push_bytes);
        return NULL;
    }

    AevkCompute* c = (AevkCompute*)calloc(1, sizeof(*c));
    if (!c) { aevk_fail(AEVK_ERR_OOM, "out of memory"); return NULL; }
    c->dev = d;
    c->push_bytes = (uint32_t)push_bytes;
    c->timeout_ns = 5000000000ull;

    VkShaderModuleCreateInfo smi = {0};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = len;
    smi.pCode = (const uint32_t*)spv;
    VkResult r = d->da.vkCreateShaderModule(d->device, &smi, NULL, &c->module);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_SHADER, "compute SPIR-V rejected (%d)", (int)r); goto fail; }

    if (bindings && bindings->count > 0) {
        /* The same declarations a graphics pipeline takes, visible to the
         * compute stage instead. */
        VkDescriptorSetLayoutBinding b[AEVK_MAX_DESC];
        uint32_t n_ub = 0, n_sb = 0, n_img = 0;
        for (uint32_t i = 0; i < bindings->count; i++) {
            b[i] = bindings->b[i];
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            c->declared[b[i].binding] = 1;
            c->desc_type[b[i].binding] = b[i].descriptorType;
            if (b[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) n_ub++;
            else if (b[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) n_sb++;
            else n_img++;
        }
        VkDescriptorSetLayoutCreateInfo dli = {0};
        dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dli.bindingCount = bindings->count;
        dli.pBindings = b;
        r = d->da.vkCreateDescriptorSetLayout(d->device, &dli, NULL, &c->set_layout);
        if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkCreateDescriptorSetLayout failed (%d)", (int)r); goto fail; }

        VkDescriptorPoolSize sizes[3];
        uint32_t nsizes = 0;
        if (n_ub)  { sizes[nsizes].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                     sizes[nsizes++].descriptorCount = n_ub; }
        if (n_sb)  { sizes[nsizes].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                     sizes[nsizes++].descriptorCount = n_sb; }
        if (n_img) { sizes[nsizes].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                     sizes[nsizes++].descriptorCount = n_img; }
        VkDescriptorPoolCreateInfo dpi = {0};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = 1;
        dpi.poolSizeCount = nsizes;
        dpi.pPoolSizes = sizes;
        r = d->da.vkCreateDescriptorPool(d->device, &dpi, NULL, &c->pool);
        if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkCreateDescriptorPool failed (%d)", (int)r); goto fail; }
        VkDescriptorSetAllocateInfo dsi = {0};
        dsi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsi.descriptorPool = c->pool;
        dsi.descriptorSetCount = 1;
        dsi.pSetLayouts = &c->set_layout;
        r = d->da.vkAllocateDescriptorSets(d->device, &dsi, &c->set);
        if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkAllocateDescriptorSets failed (%d)", (int)r); goto fail; }
    }

    VkPushConstantRange pcr = {0};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.size = (uint32_t)push_bytes;
    VkPipelineLayoutCreateInfo pli = {0};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (c->set_layout) { pli.setLayoutCount = 1; pli.pSetLayouts = &c->set_layout; }
    if (push_bytes > 0) { pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr; }
    r = d->da.vkCreatePipelineLayout(d->device, &pli, NULL, &c->layout);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkCreatePipelineLayout failed (%d)", (int)r); goto fail; }

    VkComputePipelineCreateInfo cpi = {0};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = c->module;
    cpi.stage.pName = "main";
    cpi.layout = c->layout;
    r = d->da.vkCreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &cpi, NULL, &c->pipeline);
    if (r != VK_SUCCESS) {
        aevk_fail(r == VK_ERROR_OUT_OF_HOST_MEMORY ? AEVK_ERR_OOM : AEVK_ERR_SHADER,
                  "vkCreateComputePipelines failed (%d)", (int)r);
        goto fail;
    }

    AEVK_MUTEX_LOCK(&d->lock);
    VkCommandBufferAllocateInfo cai = {0};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = d->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    r = d->da.vkAllocateCommandBuffers(d->device, &cai, &c->cmd);
    AEVK_MUTEX_UNLOCK(&d->lock);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkAllocateCommandBuffers failed (%d)", (int)r); goto fail; }
    VkFenceCreateInfo fci = {0};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    r = d->da.vkCreateFence(d->device, &fci, NULL, &c->fence);
    if (r != VK_SUCCESS) { aevk_fail(AEVK_ERR_OOM, "vkCreateFence failed (%d)", (int)r); goto fail; }
    return c;

fail:
    aevk_compute_destroy(c);
    return NULL;
}

static int aevk_compute_check_binding(AevkCompute* c, int binding) {
    if (!c) return aevk_fail(AEVK_ERR_ARG, "compute is null");
    if (binding < 0 || binding >= AEVK_MAX_DESC) {
        return aevk_fail(AEVK_ERR_ARG, "binding must be 0..%d", AEVK_MAX_DESC - 1);
    }
    if (!c->declared[binding]) return aevk_fail(AEVK_ERR_ARG, "binding %d was not declared", binding);
    if (c->submitted) {
        return aevk_fail(AEVK_ERR_ARG, "a dispatch is in flight: compute_wait before rebinding");
    }
    return AEVK_OK;
}

int aevk_compute_set_buffer(AevkCompute* c, int binding, AevkBuffer* buf) {
    aevk_clear_error();
    int rc = aevk_compute_check_binding(c, binding);
    if (rc != AEVK_OK) return rc;
    if (!buf) return aevk_fail(AEVK_ERR_ARG, "buffer is null");
    if (buf->dev != c->dev) return aevk_fail(AEVK_ERR_ARG, "the buffer belongs to another device");
    VkDescriptorType ty = c->desc_type[binding];
    if (ty != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && ty != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
        return aevk_fail(AEVK_ERR_ARG, "binding %d is declared as a texture, not a buffer", binding);
    }
    VkDescriptorBufferInfo bi = {0};
    bi.buffer = buf->buf;
    bi.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet w = {0};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = c->set;
    w.dstBinding = (uint32_t)binding;
    w.descriptorCount = 1;
    w.descriptorType = ty;
    w.pBufferInfo = &bi;
    c->dev->da.vkUpdateDescriptorSets(c->dev->device, 1, &w, 0, NULL);
    c->written[binding] = 1;
    return AEVK_OK;
}

int aevk_compute_set_texture(AevkCompute* c, int binding, AevkTexture* tex) {
    aevk_clear_error();
    int rc = aevk_compute_check_binding(c, binding);
    if (rc != AEVK_OK) return rc;
    if (!tex) return aevk_fail(AEVK_ERR_ARG, "texture is null");
    if (tex->dev != c->dev) return aevk_fail(AEVK_ERR_ARG, "the texture belongs to another device");
    if (c->desc_type[binding] != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
        return aevk_fail(AEVK_ERR_ARG, "binding %d is declared as a buffer, not a texture", binding);
    }
    if (!tex->uploaded) {
        return aevk_fail(AEVK_ERR_ARG, "texture has no pixels yet, upload before binding");
    }
    VkDescriptorImageInfo ii = {0};
    ii.sampler = tex->sampler;
    ii.imageView = tex->view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w = {0};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = c->set;
    w.dstBinding = (uint32_t)binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    c->dev->da.vkUpdateDescriptorSets(c->dev->device, 1, &w, 0, NULL);
    c->written[binding] = 1;
    return AEVK_OK;
}

int aevk_compute_set_push(AevkCompute* c, const void* data, size_t len) {
    aevk_clear_error();
    if (!c) return aevk_fail(AEVK_ERR_ARG, "compute is null");
    if (len > c->push_bytes) {
        return aevk_fail(AEVK_ERR_ARG, "the pipeline declared %u push bytes, got %zu",
                         c->push_bytes, len);
    }
    if (len > 0 && !data) return aevk_fail(AEVK_ERR_ARG, "push data is null");
    if (len > 0) memcpy(c->push, data, len);
    return AEVK_OK;
}

int aevk_compute_set_timeout_ms(AevkCompute* c, int ms) {
    aevk_clear_error();
    if (!c) return aevk_fail(AEVK_ERR_ARG, "compute is null");
    if (ms <= 0) return aevk_fail(AEVK_ERR_ARG, "timeout must be positive (got %d)", ms);
    c->timeout_ns = (uint64_t)ms * 1000000ull;
    return AEVK_OK;
}

/* THE DEVICE LOCK IS HELD. */
static int aevk_compute_wait_locked(AevkCompute* c) {
    if (!c->submitted) return AEVK_OK;
    AevkDevice* d = c->dev;
    VkResult vr = d->da.vkWaitForFences(d->device, 1, &c->fence, VK_TRUE, c->timeout_ns);
    if (vr == VK_TIMEOUT) {
        return aevk_fail(AEVK_ERR_DEVICE_LOST, "the dispatch did not finish within %llu ms",
                         (unsigned long long)(c->timeout_ns / 1000000ull));
    }
    if (vr != VK_SUCCESS) {
        return aevk_fail(vr == VK_ERROR_DEVICE_LOST ? AEVK_ERR_DEVICE_LOST : AEVK_ERR_OOM,
                         "vkWaitForFences failed (%d)", (int)vr);
    }
    c->submitted = 0;
    return AEVK_OK;
}

/* Records and submits one dispatch of gx * gy * gz work groups without
 * waiting. The buffers it writes are readable once compute_wait returns. */
int aevk_dispatch_async(AevkCompute* c, int gx, int gy, int gz) {
    aevk_clear_error();
    if (!c) return aevk_fail(AEVK_ERR_ARG, "compute is null");
    AevkDevice* d = c->dev;
    if (gx <= 0 || gy <= 0 || gz <= 0) {
        return aevk_fail(AEVK_ERR_ARG, "work group counts must be positive, got %d x %d x %d",
                         gx, gy, gz);
    }
    if ((uint32_t)gx > d->max_groups[0] || (uint32_t)gy > d->max_groups[1] ||
        (uint32_t)gz > d->max_groups[2]) {
        return aevk_fail(AEVK_ERR_UNSUPPORTED,
                         "%d x %d x %d work groups exceeds the device limit of %u x %u x %u",
                         gx, gy, gz, d->max_groups[0], d->max_groups[1], d->max_groups[2]);
    }
    /* A descriptor never written has no defined contents, and a software
     * rasteriser dereferences it as it is bound: refuse, and say which. */
    for (int i = 0; i < AEVK_MAX_DESC; i++) {
        if (c->declared[i] && !c->written[i]) {
            return aevk_fail(AEVK_ERR_ARG, "binding %d was declared but never set", i);
        }
    }

    AEVK_MUTEX_LOCK(&d->lock);
    int rc = aevk_compute_wait_locked(c);
    if (rc != AEVK_OK) { AEVK_MUTEX_UNLOCK(&d->lock); return rc; }

    VkCommandBuffer cmd = c->cmd;
    VkResult vr = d->da.vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vr == VK_SUCCESS) vr = d->da.vkBeginCommandBuffer(cmd, &bi);
    if (vr != VK_SUCCESS) {
        AEVK_MUTEX_UNLOCK(&d->lock);
        return aevk_fail(AEVK_ERR_OOM, "cannot begin the dispatch (%d)", (int)vr);
    }

    /* Earlier work on the queue that read or wrote these buffers (a draw
     * pulling vertices from one, the previous dispatch) finishes first. */
    VkMemoryBarrier before = {0};
    before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    d->da.vkCmdPipelineBarrier(cmd,
                               VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                               VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               0, 1, &before, 0, NULL, 0, NULL);
    d->da.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->pipeline);
    if (c->set) {
        d->da.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->layout, 0, 1,
                                      &c->set, 0, NULL);
    }
    if (c->push_bytes > 0) {
        d->da.vkCmdPushConstants(cmd, c->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                 c->push_bytes, c->push);
    }
    d->da.vkCmdDispatch(cmd, (uint32_t)gx, (uint32_t)gy, (uint32_t)gz);
    /* The results are for the host (read back after the fence) and for any
     * later draw that reads the buffers as vertices, indices, uniforms or
     * storage. */
    VkMemoryBarrier after = {0};
    after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                          VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                          VK_ACCESS_UNIFORM_READ_BIT;
    d->da.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_HOST_BIT |
                               VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                               VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               0, 1, &after, 0, NULL, 0, NULL);
    vr = d->da.vkEndCommandBuffer(cmd);
    if (vr == VK_SUCCESS) vr = d->da.vkResetFences(d->device, 1, &c->fence);
    if (vr == VK_SUCCESS) {
        VkSubmitInfo si = {0};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vr = d->da.vkQueueSubmit(d->queue, 1, &si, c->fence);
    }
    if (vr == VK_SUCCESS) c->submitted = 1;
    AEVK_MUTEX_UNLOCK(&d->lock);
    if (vr != VK_SUCCESS) {
        return aevk_fail(vr == VK_ERROR_DEVICE_LOST ? AEVK_ERR_DEVICE_LOST : AEVK_ERR_OOM,
                         "the dispatch could not be submitted (%d)", (int)vr);
    }
    return AEVK_OK;
}

int aevk_compute_wait(AevkCompute* c) {
    aevk_clear_error();
    if (!c) return aevk_fail(AEVK_ERR_ARG, "compute is null");
    AEVK_MUTEX_LOCK(&c->dev->lock);
    int rc = aevk_compute_wait_locked(c);
    AEVK_MUTEX_UNLOCK(&c->dev->lock);
    return rc;
}

int aevk_dispatch(AevkCompute* c, int gx, int gy, int gz) {
    int rc = aevk_dispatch_async(c, gx, gy, gz);
    if (rc != AEVK_OK) return rc;
    return aevk_compute_wait(c);
}

/* ------------------------------------------------------------------------ */
/* Presentation (#1505)                                                      */
/* ------------------------------------------------------------------------ */

/* Frames the presentation path keeps in flight: the CPU records and submits
 * frame N+1 while the GPU copies frame N into its swapchain image. */
#define AEVK_PRESENT_FRAMES 2

typedef struct {
    VkCommandBuffer cmd;
    VkFence         fence;
    VkSemaphore     acquired;   /* signalled when the image is ours to write */
    int             submitted;
} AevkPresentFrame;

struct AevkSwapchain {
    AevkDevice*      dev;
    int              kind;
    VkSurfaceKHR     surface;
    VkSwapchainKHR   swapchain;
    VkFormat         format;
    VkColorSpaceKHR  color_space;
    VkFormatFeatureFlags features;   /* optimal-tiling features of `format` */
    int              srgb;           /* the encoding `format` was chosen for */
    uint32_t         width, height;  /* 0 x 0 while the window has no area */
    uint32_t         want_w, want_h;
    int              vsync;
    int              stale;          /* rebuild before the next acquire */
    uint32_t         image_count;
    VkImage*         images;
    /* One per swapchain IMAGE, not per frame: the semaphore a present waits
     * on may only be reused once that image has been acquired again, which is
     * what indexing by image guarantees and indexing by frame does not. */
    VkSemaphore*     rendered;
    AevkPresentFrame frames[AEVK_PRESENT_FRAMES];
    int              frame;
    uint64_t         timeout_ns;
    long long        presented;
};

static int aevk_format_is_srgb(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB;
}

/* An sRGB target's bytes are already display-encoded; so are a UNORM
 * target's, by the convention that shaders writing to one write display
 * values. A float target holds linear light, which an sRGB swapchain encodes
 * on the way in; a UNORM one would show it too dark. So the swapchain's
 * encoding follows the target's: sRGB for sRGB and float, UNORM for UNORM. */
static int aevk_target_wants_srgb_display(VkFormat f) {
    return aevk_format_is_srgb(f) ||
           f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_R32G32B32A32_SFLOAT;
}

#if defined(AEVK_HAVE_METAL_SURFACE)
#include <pthread.h>
#include <objc/runtime.h>

/* An NSView presents through a CAMetalLayer, and AppKit gives a view one only
 * when asked. Done through the Objective-C runtime so this file stays C and
 * needs no framework on the link line: libobjc and QuartzCore are opened the
 * way the loader is. Returns the view's layer (made and attached when it had
 * none) or NULL with the reason set. */
static void* aevk_metal_layer_for_view(void* view) {
    static void* objc;
    static void* quartz;
    if (!objc) objc = AEVK_DLOPEN("/usr/lib/libobjc.A.dylib");
    if (!quartz) quartz = AEVK_DLOPEN("/System/Library/Frameworks/QuartzCore.framework/QuartzCore");
    if (!objc || !quartz) {
        aevk_fail(AEVK_ERR_UNSUPPORTED, "cannot load libobjc or QuartzCore");
        return NULL;
    }
    Class (*get_class)(const char*) = (Class (*)(const char*))AEVK_DLSYM(objc, "objc_getClass");
    SEL (*sel)(const char*) = (SEL (*)(const char*))AEVK_DLSYM(objc, "sel_registerName");
    void* send = AEVK_DLSYM(objc, "objc_msgSend");
    void* (*pool_push)(void) = (void* (*)(void))AEVK_DLSYM(objc, "objc_autoreleasePoolPush");
    void (*pool_pop)(void*) = (void (*)(void*))AEVK_DLSYM(objc, "objc_autoreleasePoolPop");
    if (!get_class || !sel || !send || !pool_push || !pool_pop) {
        aevk_fail(AEVK_ERR_UNSUPPORTED, "the Objective-C runtime is missing an entry point");
        return NULL;
    }
    Class metal_layer = get_class("CAMetalLayer");
    if (!metal_layer) {
        aevk_fail(AEVK_ERR_UNSUPPORTED, "QuartzCore has no CAMetalLayer");
        return NULL;
    }

    void* pool = pool_push();
    id v = (id)view;
    id layer = ((id (*)(id, SEL))send)(v, sel("layer"));
    if (!layer || !((BOOL (*)(id, SEL, Class))send)(layer, sel("isKindOfClass:"), metal_layer)) {
        /* Layer-HOSTING, not layer-backed: set the layer before asking for
         * one, so AppKit uses ours and never draws into it. */
        layer = ((id (*)(id, SEL))send)((id)metal_layer, sel("layer"));
        ((void (*)(id, SEL, id))send)(v, sel("setLayer:"), layer);
        ((void (*)(id, SEL, BOOL))send)(v, sel("setWantsLayer:"), (BOOL)1);
    }
    /* Pixels, not points: without the backing scale a Retina window gets a
     * swapchain at half its resolution. */
    id win = ((id (*)(id, SEL))send)(v, sel("window"));
    if (win) {
        double scale = ((double (*)(id, SEL))send)(win, sel("backingScaleFactor"));
        if (scale > 0.0) ((void (*)(id, SEL, double))send)(layer, sel("setContentsScale:"), scale);
    }
    pool_pop(pool);   /* the view holds the layer now */
    return (void*)layer;
}
#endif

static const char* aevk_window_kind_name(int kind) {
    switch (kind) {
        case AEVK_WINDOW_WIN32:       return "a Win32 window (VK_KHR_win32_surface)";
        case AEVK_WINDOW_NSVIEW:      return "an NSView (VK_EXT_metal_surface)";
        case AEVK_WINDOW_X11:         return "an X11 window (VK_KHR_xlib_surface)";
        case AEVK_WINDOW_WAYLAND:     return "a Wayland surface (VK_KHR_wayland_surface)";
        case AEVK_WINDOW_METAL_LAYER: return "a CAMetalLayer (VK_EXT_metal_surface)";
        default:                      return "an unknown kind of window";
    }
}

/* The surface for one window, by kind. The device lock need not be held: a
 * surface belongs to the instance, and creating one touches no queue. */
static int aevk_make_surface(AevkDevice* d, int kind, void* display, void* window,
                             VkSurfaceKHR* out) {
    /* Only the X11 and Wayland kinds take a display connection. */
    (void)display;
    if (kind < AEVK_WINDOW_WIN32 || kind > AEVK_WINDOW_METAL_LAYER) {
        return aevk_fail(AEVK_ERR_ARG, "window kind %d is not one of 1..5", kind);
    }
    if (!(d->surface_kinds & (1u << kind))) {
        return aevk_fail(AEVK_ERR_UNSUPPORTED, "this build and loader cannot present to %s",
                         aevk_window_kind_name(kind));
    }
    if (!window) return aevk_fail(AEVK_ERR_ARG, "window handle is null");
    VkResult r = VK_ERROR_EXTENSION_NOT_PRESENT;
    switch (kind) {
#if defined(AEVK_HAVE_WIN32_SURFACE)
        case AEVK_WINDOW_WIN32: {
            VkWin32SurfaceCreateInfoKHR ci = {0};
            ci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
            ci.hinstance = (HINSTANCE)GetWindowLongPtrW((HWND)window, GWLP_HINSTANCE);
            ci.hwnd = (HWND)window;
            r = d->ia.vkCreateWin32SurfaceKHR(d->instance, &ci, NULL, out);
            break;
        }
#endif
#if defined(AEVK_HAVE_METAL_SURFACE)
        case AEVK_WINDOW_NSVIEW:
        case AEVK_WINDOW_METAL_LAYER: {
            void* layer = window;
            if (kind == AEVK_WINDOW_NSVIEW) {
                if (!pthread_main_np()) {
                    return aevk_fail(AEVK_ERR_ARG,
                                     "an NSView can only be given a Metal layer on the main thread");
                }
                layer = aevk_metal_layer_for_view(window);
                if (!layer) return AEVK_ERR_UNSUPPORTED;
            }
            VkMetalSurfaceCreateInfoEXT ci = {0};
            ci.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
            ci.pLayer = (const CAMetalLayer*)layer;
            r = d->ia.vkCreateMetalSurfaceEXT(d->instance, &ci, NULL, out);
            break;
        }
#endif
#if defined(AEVK_HAVE_WAYLAND_SURFACE)
        case AEVK_WINDOW_WAYLAND: {
            if (!display) return aevk_fail(AEVK_ERR_ARG, "a Wayland surface needs its wl_display");
            VkWaylandSurfaceCreateInfoKHR ci = {0};
            ci.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
            ci.display = (struct wl_display*)display;
            ci.surface = (struct wl_surface*)window;
            r = d->ia.vkCreateWaylandSurfaceKHR(d->instance, &ci, NULL, out);
            break;
        }
#endif
#if defined(AEVK_HAVE_XLIB_SURFACE)
        case AEVK_WINDOW_X11: {
            if (!display) return aevk_fail(AEVK_ERR_ARG, "an X11 window needs its Display");
            VkXlibSurfaceCreateInfoKHR ci = {0};
            ci.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
            ci.dpy = (Display*)display;
            ci.window = (Window)(uintptr_t)window;
            r = d->ia.vkCreateXlibSurfaceKHR(d->instance, &ci, NULL, out);
            break;
        }
#endif
        default:
            break;
    }
    if (r != VK_SUCCESS) {
        return aevk_fail(AEVK_ERR_UNSUPPORTED, "cannot make a surface for %s (VkResult %d)",
                         aevk_window_kind_name(kind), (int)r);
    }
    return AEVK_OK;
}

/* The image format, from what the surface offers: 8-bit BGRA or RGBA in the
 * encoding the presented target uses, preferring one the device can blit
 * into, since that is how a frame reaches it. A surface offering neither
 * (possible, if unusual) gets its first format and a blit that converts. */
static int aevk_sc_pick_format(AevkSwapchain* sc) {
    AevkDevice* d = sc->dev;
    uint32_t n = 0;
    VkResult r = d->ia.vkGetPhysicalDeviceSurfaceFormatsKHR(d->phys, sc->surface, &n, NULL);
    if (r != VK_SUCCESS || n == 0) {
        return aevk_fail(AEVK_ERR_UNSUPPORTED, "the surface offers no formats (%d)", (int)r);
    }
    VkSurfaceFormatKHR* fmts = (VkSurfaceFormatKHR*)calloc(n, sizeof(*fmts));
    if (!fmts) return aevk_fail(AEVK_ERR_OOM, "out of memory");
    r = d->ia.vkGetPhysicalDeviceSurfaceFormatsKHR(d->phys, sc->surface, &n, fmts);
    if (r != VK_SUCCESS || n == 0) {
        free(fmts);
        return aevk_fail(AEVK_ERR_UNSUPPORTED, "the surface offers no formats (%d)", (int)r);
    }

    const VkFormat wanted[2] = {
        sc->srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM,
        sc->srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
    };
    VkFormat pick = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    if (n == 1 && fmts[0].format == VK_FORMAT_UNDEFINED) {
        /* "Any format": the surface imposes none. */
        pick = wanted[0];
        space = fmts[0].colorSpace;
    }
    for (int pass = 0; pass < 2 && pick == VK_FORMAT_UNDEFINED; pass++) {
        for (int w = 0; w < 2 && pick == VK_FORMAT_UNDEFINED; w++) {
            for (uint32_t i = 0; i < n; i++) {
                if (fmts[i].format != wanted[w] ||
                    fmts[i].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
                VkFormatProperties fp;
                d->ia.vkGetPhysicalDeviceFormatProperties(d->phys, fmts[i].format, &fp);
                if (pass == 0 && !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) continue;
                pick = fmts[i].format;
                space = fmts[i].colorSpace;
                break;
            }
        }
    }
    if (pick == VK_FORMAT_UNDEFINED) {
        pick = fmts[0].format;
        space = fmts[0].colorSpace;
    }
    free(fmts);

    VkFormatProperties fp;
    d->ia.vkGetPhysicalDeviceFormatProperties(d->phys, pick, &fp);
    sc->format = pick;
    sc->color_space = space;
    sc->features = fp.optimalTilingFeatures;
    return AEVK_OK;
}

static void aevk_sc_free_images(AevkSwapchain* sc) {
    AevkDevice* d = sc->dev;
    if (sc->rendered) {
        for (uint32_t i = 0; i < sc->image_count; i++) {
            if (sc->rendered[i]) d->da.vkDestroySemaphore(d->device, sc->rendered[i], NULL);
        }
    }
    free(sc->rendered);
    free(sc->images);
    sc->rendered = NULL;
    sc->images = NULL;
    sc->image_count = 0;
}

/* (Re)creates the swapchain at the surface's current size. THE DEVICE LOCK
 * MUST BE HELD and the device idle: the old images, and the semaphores the
 * presentation engine waits on, are released here.
 *
 * A window with no area (minimised) leaves the swapchain absent and the size
 * 0 x 0, which aevk_present treats as "nothing to show" rather than as an
 * error. */
static int aevk_sc_build(AevkSwapchain* sc) {
    AevkDevice* d = sc->dev;
    int rc = aevk_sc_pick_format(sc);
    if (rc != AEVK_OK) return rc;

    VkSurfaceCapabilitiesKHR caps;
    VkResult r = d->ia.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d->phys, sc->surface, &caps);
    if (r != VK_SUCCESS) {
        return aevk_fail(r == VK_ERROR_SURFACE_LOST_KHR ? AEVK_ERR_DEVICE_LOST : AEVK_ERR_UNSUPPORTED,
                         "cannot read the surface's capabilities (%d)", (int)r);
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        /* The window system leaves the size to us (Wayland). */
        extent.width = sc->want_w;
        extent.height = sc->want_h;
        if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
        if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
        if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }

    VkSwapchainKHR old = sc->swapchain;
    if (extent.width == 0 || extent.height == 0) {
        if (old) d->da.vkDestroySwapchainKHR(d->device, old, NULL);
        aevk_sc_free_images(sc);
        sc->swapchain = VK_NULL_HANDLE;
        sc->width = 0;
        sc->height = 0;
        sc->stale = 0;
        return AEVK_OK;
    }

    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        return aevk_fail(AEVK_ERR_UNSUPPORTED,
                         "the surface's images cannot be written by a transfer");
    }

    /* One more than the minimum, so acquiring never has to wait for the
     * presentation engine to release the image on screen. */
    uint32_t count = caps.minImageCount + 1;
    if (count < 2) count = 2;
    if (caps.maxImageCount > 0 && count > caps.maxImageCount) count = caps.maxImageCount;

    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;   /* always offered */
    if (!sc->vsync) {
        uint32_t nm = 0;
        if (d->ia.vkGetPhysicalDeviceSurfacePresentModesKHR(d->phys, sc->surface, &nm, NULL) == VK_SUCCESS &&
            nm > 0) {
            VkPresentModeKHR* modes = (VkPresentModeKHR*)calloc(nm, sizeof(*modes));
            if (!modes) return aevk_fail(AEVK_ERR_OOM, "out of memory");
            if (d->ia.vkGetPhysicalDeviceSurfacePresentModesKHR(d->phys, sc->surface, &nm, modes) == VK_SUCCESS) {
                int mailbox = 0, immediate = 0;
                for (uint32_t i = 0; i < nm; i++) {
                    if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mailbox = 1;
                    if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) immediate = 1;
                }
                if (mailbox) mode = VK_PRESENT_MODE_MAILBOX_KHR;
                else if (immediate) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            }
            free(modes);
        }
    }

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        const VkCompositeAlphaFlagBitsKHR order[3] = {
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        };
        for (int i = 0; i < 3; i++) {
            if (caps.supportedCompositeAlpha & order[i]) { alpha = order[i]; break; }
        }
    }

    VkSwapchainCreateInfoKHR ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = sc->surface;
    ci.minImageCount = count;
    ci.imageFormat = sc->format;
    ci.imageColorSpace = sc->color_space;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;

    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    r = d->da.vkCreateSwapchainKHR(d->device, &ci, NULL, &fresh);
    /* The old swapchain is retired either way: after a successful create it
     * is superseded, and after a failed one it was still passed as
     * oldSwapchain, which retires it too. */
    if (old) d->da.vkDestroySwapchainKHR(d->device, old, NULL);
    sc->swapchain = VK_NULL_HANDLE;
    aevk_sc_free_images(sc);
    if (r != VK_SUCCESS) {
        sc->width = 0;
        sc->height = 0;
        return aevk_fail(r == VK_ERROR_OUT_OF_HOST_MEMORY || r == VK_ERROR_OUT_OF_DEVICE_MEMORY
                             ? AEVK_ERR_OOM : AEVK_ERR_UNSUPPORTED,
                         "vkCreateSwapchainKHR failed (%d)", (int)r);
    }
    sc->swapchain = fresh;

    uint32_t n = 0;
    r = d->da.vkGetSwapchainImagesKHR(d->device, fresh, &n, NULL);
    if (r != VK_SUCCESS || n == 0) {
        return aevk_fail(AEVK_ERR_UNSUPPORTED, "vkGetSwapchainImagesKHR failed (%d)", (int)r);
    }
    sc->images = (VkImage*)calloc(n, sizeof(VkImage));
    sc->rendered = (VkSemaphore*)calloc(n, sizeof(VkSemaphore));
    if (!sc->images || !sc->rendered) {
        aevk_sc_free_images(sc);
        return aevk_fail(AEVK_ERR_OOM, "out of memory");
    }
    sc->image_count = n;
    r = d->da.vkGetSwapchainImagesKHR(d->device, fresh, &n, sc->images);
    if (r != VK_SUCCESS) {
        return aevk_fail(AEVK_ERR_UNSUPPORTED, "vkGetSwapchainImagesKHR failed (%d)", (int)r);
    }
    for (uint32_t i = 0; i < sc->image_count; i++) {
        VkSemaphoreCreateInfo si = {0};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        r = d->da.vkCreateSemaphore(d->device, &si, NULL, &sc->rendered[i]);
        if (r != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkCreateSemaphore failed (%d)", (int)r);
    }
    sc->width = extent.width;
    sc->height = extent.height;
    sc->stale = 0;
    return AEVK_OK;
}

/* Rebuild with the device idle, which is what makes releasing the old images
 * and their semaphores safe. THE DEVICE LOCK MUST BE HELD. */
static int aevk_sc_rebuild(AevkSwapchain* sc) {
    AevkDevice* d = sc->dev;
    d->da.vkDeviceWaitIdle(d->device);
    for (int i = 0; i < AEVK_PRESENT_FRAMES; i++) sc->frames[i].submitted = 0;
    return aevk_sc_build(sc);
}

void aevk_swapchain_destroy(AevkSwapchain* sc) {
    if (!sc) return;
    AevkDevice* d = sc->dev;
    if (d && d->device) {
        AEVK_MUTEX_LOCK(&d->lock);
        d->da.vkDeviceWaitIdle(d->device);
        for (int i = 0; i < AEVK_PRESENT_FRAMES; i++) {
            AevkPresentFrame* f = &sc->frames[i];
            if (f->acquired) d->da.vkDestroySemaphore(d->device, f->acquired, NULL);
            if (f->fence)    d->da.vkDestroyFence(d->device, f->fence, NULL);
            if (f->cmd)      d->da.vkFreeCommandBuffers(d->device, d->pool, 1, &f->cmd);
        }
        aevk_sc_free_images(sc);
        if (sc->swapchain) d->da.vkDestroySwapchainKHR(d->device, sc->swapchain, NULL);
        AEVK_MUTEX_UNLOCK(&d->lock);
        if (sc->surface) d->ia.vkDestroySurfaceKHR(d->instance, sc->surface, NULL);
    }
    free(sc);
}

AevkSwapchain* aevk_swapchain_create(AevkDevice* d, int kind, void* display,
                                     void* window, int width, int height) {
    aevk_clear_error();
    if (!d) { aevk_fail(AEVK_ERR_ARG, "device is null"); return NULL; }
    if (width < 0 || height < 0) {
        aevk_fail(AEVK_ERR_ARG, "size must not be negative, got %dx%d", width, height);
        return NULL;
    }
    if (!d->surface_kinds) {
        aevk_fail(AEVK_ERR_UNSUPPORTED, "the Vulkan loader offers no VK_KHR_surface");
        return NULL;
    }
    if (!d->can_present) {
        aevk_fail(AEVK_ERR_UNSUPPORTED, "the device offers no VK_KHR_swapchain");
        return NULL;
    }

    AevkSwapchain* sc = (AevkSwapchain*)calloc(1, sizeof(*sc));
    if (!sc) { aevk_fail(AEVK_ERR_OOM, "out of memory"); return NULL; }
    sc->dev = d;
    sc->kind = kind;
    sc->want_w = (uint32_t)width;
    sc->want_h = (uint32_t)height;
    sc->vsync = 1;
    sc->timeout_ns = 5000000000ull;

    if (aevk_make_surface(d, kind, display, window, &sc->surface) != AEVK_OK) {
        free(sc);
        return NULL;
    }

    /* The device's graphics queue has to be able to present to THIS surface:
     * the queue is chosen before any window exists, so this is where it is
     * checked. Every desktop driver presents from its graphics queue. */
    VkBool32 ok = VK_FALSE;
    VkResult r = d->ia.vkGetPhysicalDeviceSurfaceSupportKHR(d->phys, d->queue_family,
                                                            sc->surface, &ok);
    if (r != VK_SUCCESS || !ok) {
        aevk_fail(AEVK_ERR_UNSUPPORTED,
                  "the device's graphics queue cannot present to this window (%d)", (int)r);
        aevk_swapchain_destroy(sc);
        return NULL;
    }

    AEVK_MUTEX_LOCK(&d->lock);
    int rc = AEVK_OK;
    for (int i = 0; i < AEVK_PRESENT_FRAMES && rc == AEVK_OK; i++) {
        AevkPresentFrame* f = &sc->frames[i];
        VkCommandBufferAllocateInfo cai = {0};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = d->pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        r = d->da.vkAllocateCommandBuffers(d->device, &cai, &f->cmd);
        if (r != VK_SUCCESS) { rc = aevk_fail(AEVK_ERR_OOM, "vkAllocateCommandBuffers failed (%d)", (int)r); break; }
        VkFenceCreateInfo fci = {0};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        r = d->da.vkCreateFence(d->device, &fci, NULL, &f->fence);
        if (r != VK_SUCCESS) { rc = aevk_fail(AEVK_ERR_OOM, "vkCreateFence failed (%d)", (int)r); break; }
        VkSemaphoreCreateInfo si = {0};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        r = d->da.vkCreateSemaphore(d->device, &si, NULL, &f->acquired);
        if (r != VK_SUCCESS) { rc = aevk_fail(AEVK_ERR_OOM, "vkCreateSemaphore failed (%d)", (int)r); break; }
    }
    if (rc == AEVK_OK) rc = aevk_sc_build(sc);
    AEVK_MUTEX_UNLOCK(&d->lock);
    if (rc != AEVK_OK) {
        /* Keep the reason: destroying runs no failing call, but be explicit
         * that the message is the build's. */
        char reason[sizeof(g_err)];
        snprintf(reason, sizeof(reason), "%s", g_err);
        aevk_swapchain_destroy(sc);
        snprintf(g_err, sizeof(g_err), "%s", reason);
        return NULL;
    }
    return sc;
}

int aevk_swapchain_resize(AevkSwapchain* sc, int width, int height) {
    aevk_clear_error();
    if (!sc) return aevk_fail(AEVK_ERR_ARG, "swapchain is null");
    if (width < 0 || height < 0) {
        return aevk_fail(AEVK_ERR_ARG, "size must not be negative, got %dx%d", width, height);
    }
    AevkDevice* d = sc->dev;
    AEVK_MUTEX_LOCK(&d->lock);
    sc->want_w = (uint32_t)width;
    sc->want_h = (uint32_t)height;
    int rc = aevk_sc_rebuild(sc);
    AEVK_MUTEX_UNLOCK(&d->lock);
    return rc;
}

int aevk_swapchain_set_vsync(AevkSwapchain* sc, int on) {
    aevk_clear_error();
    if (!sc) return aevk_fail(AEVK_ERR_ARG, "swapchain is null");
    on = on ? 1 : 0;
    if (on == sc->vsync) return AEVK_OK;
    AevkDevice* d = sc->dev;
    AEVK_MUTEX_LOCK(&d->lock);
    sc->vsync = on;
    int rc = aevk_sc_rebuild(sc);
    AEVK_MUTEX_UNLOCK(&d->lock);
    return rc;
}

int aevk_swapchain_width(const AevkSwapchain* sc)  { return sc ? (int)sc->width : 0; }
int aevk_swapchain_height(const AevkSwapchain* sc) { return sc ? (int)sc->height : 0; }
int aevk_swapchain_format(const AevkSwapchain* sc) { return sc ? (int)sc->format : 0; }
long long aevk_swapchain_presented(const AevkSwapchain* sc) { return sc ? sc->presented : 0; }

/* How a frame gets from the target into a swapchain image: a blit when the
 * device can blit between the two formats (and filter, when the sizes
 * differ), a plain copy when the formats and sizes are identical, and
 * otherwise nothing, which is reported before an image is acquired so no
 * acquired image is ever left unpresented. 1 blit, 2 copy, 0 impossible. */
static int aevk_sc_transfer_kind(AevkSwapchain* sc, AevkTarget* t, int* linear) {
    AevkDevice* d = sc->dev;
    VkFormatProperties src;
    d->ia.vkGetPhysicalDeviceFormatProperties(d->phys, t->color_format, &src);
    int same_size = (uint32_t)t->width == sc->width && (uint32_t)t->height == sc->height;
    /* A scaled blit filters linearly where the source format allows it and
     * picks the nearest texel where it does not (some drivers cannot filter
     * 32-bit float): a blockier scale beats refusing to show the frame. */
    *linear = !same_size &&
              (src.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    if ((src.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
        (sc->features & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        return 1;
    }
    if (same_size && sc->format == t->color_format) return 2;
    return 0;
}

/* THE DEVICE LOCK IS HELD. */
static int aevk_present_locked(AevkSwapchain* sc, AevkTarget* t) {
    AevkDevice* d = sc->dev;

    int want_srgb = aevk_target_wants_srgb_display(t->color_format);
    if (want_srgb != sc->srgb) {
        sc->srgb = want_srgb;
        sc->stale = 1;
    }
    /* The window's size now, against the swapchain's. Drivers are not
     * required to report a swapchain out of date when its window resizes or
     * is minimised (NVIDIA's keeps acquiring and presenting into a minimised
     * Win32 window at the old size), so the surface is asked every frame
     * rather than trusting acquire and present to say so. Where the window
     * system leaves the size to the application (0xFFFFFFFF, Wayland) there is
     * nothing to compare, and swapchain_resize is how it changes. */
    VkSurfaceCapabilitiesKHR caps;
    VkResult cr = d->ia.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d->phys, sc->surface, &caps);
    if (cr == VK_ERROR_SURFACE_LOST_KHR) {
        return aevk_fail(AEVK_ERR_DEVICE_LOST, "the window's surface is gone");
    }
    if (cr == VK_SUCCESS && caps.currentExtent.width != 0xFFFFFFFFu &&
        (caps.currentExtent.width != sc->width || caps.currentExtent.height != sc->height)) {
        sc->stale = 1;
    }

    /* A swapchain that went stale is rebuilt, and so is an absent one: the
     * window had no area last time and may have one now. */
    if (sc->stale || !sc->swapchain) {
        int rc = aevk_sc_rebuild(sc);
        if (rc != AEVK_OK) return rc;
        if (!sc->swapchain) return AEVK_OK;   /* still no area: nothing to show */
    }

    int linear = 0;
    int how = aevk_sc_transfer_kind(sc, t, &linear);
    if (!how) {
        return aevk_fail(AEVK_ERR_UNSUPPORTED,
                         "the device can neither blit nor copy format %d into the swapchain's %d",
                         (int)t->color_format, (int)sc->format);
    }

    AevkPresentFrame* f = &sc->frames[sc->frame];
    if (f->submitted) {
        VkResult wr = d->da.vkWaitForFences(d->device, 1, &f->fence, VK_TRUE, sc->timeout_ns);
        if (wr == VK_TIMEOUT) {
            return aevk_fail(AEVK_ERR_DEVICE_LOST, "the GPU did not finish a present within %llu ms",
                             (unsigned long long)(sc->timeout_ns / 1000000ull));
        }
        if (wr != VK_SUCCESS) {
            return aevk_fail(wr == VK_ERROR_DEVICE_LOST ? AEVK_ERR_DEVICE_LOST : AEVK_ERR_OOM,
                             "vkWaitForFences failed (%d)", (int)wr);
        }
        f->submitted = 0;
    }

    uint32_t idx = 0;
    VkResult vr = d->da.vkAcquireNextImageKHR(d->device, sc->swapchain, sc->timeout_ns,
                                              f->acquired, VK_NULL_HANDLE, &idx);
    if (vr == VK_ERROR_OUT_OF_DATE_KHR) {
        int rc = aevk_sc_rebuild(sc);
        if (rc != AEVK_OK) return rc;
        if (!sc->swapchain) return AEVK_OK;
        how = aevk_sc_transfer_kind(sc, t, &linear);
        if (!how) {
            return aevk_fail(AEVK_ERR_UNSUPPORTED,
                             "the device can neither blit nor copy format %d into the swapchain's %d",
                             (int)t->color_format, (int)sc->format);
        }
        vr = d->da.vkAcquireNextImageKHR(d->device, sc->swapchain, sc->timeout_ns,
                                         f->acquired, VK_NULL_HANDLE, &idx);
    }
    if (vr == VK_SUBOPTIMAL_KHR) {
        /* Usable, but the window no longer matches it: this frame goes out,
         * the next one gets a rebuilt swapchain. */
        sc->stale = 1;
    } else if (vr != VK_SUCCESS) {
        if (vr == VK_TIMEOUT || vr == VK_NOT_READY) {
            return aevk_fail(AEVK_ERR_DEVICE_LOST, "no swapchain image became available within %llu ms",
                             (unsigned long long)(sc->timeout_ns / 1000000ull));
        }
        if (vr == VK_ERROR_SURFACE_LOST_KHR) {
            return aevk_fail(AEVK_ERR_DEVICE_LOST, "the window's surface is gone");
        }
        return aevk_fail(vr == VK_ERROR_DEVICE_LOST ? AEVK_ERR_DEVICE_LOST : AEVK_ERR_OOM,
                         "vkAcquireNextImageKHR failed (%d)", (int)vr);
    }

    /* From here the image is ours and has to be presented, so nothing below
     * returns early except on a device-level failure. */
    VkImage dst = sc->images[idx];
    VkCommandBuffer cmd = f->cmd;
    vr = d->da.vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vr == VK_SUCCESS) vr = d->da.vkBeginCommandBuffer(cmd, &bi);
    if (vr != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "cannot begin the present commands (%d)", (int)vr);

    /* The whole image is overwritten, so its old contents are discarded
     * (UNDEFINED). The source needs no barrier: the render pass left it in
     * TRANSFER_SRC_OPTIMAL, and its external dependency already orders the
     * colour writes before transfer reads later on the queue. */
    aevk_image_barrier(d, cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       0, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (how == 1) {
        VkImageBlit blit = {0};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1].x = t->width;
        blit.srcOffsets[1].y = t->height;
        blit.srcOffsets[1].z = 1;
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[1].x = (int32_t)sc->width;
        blit.dstOffsets[1].y = (int32_t)sc->height;
        blit.dstOffsets[1].z = 1;
        d->da.vkCmdBlitImage(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                             linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    } else {
        VkImageCopy copy = {0};
        copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.srcSubresource.layerCount = 1;
        copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.dstSubresource.layerCount = 1;
        copy.extent.width = sc->width;
        copy.extent.height = sc->height;
        copy.extent.depth = 1;
        d->da.vkCmdCopyImage(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    aevk_image_barrier(d, cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                       VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vr = d->da.vkEndCommandBuffer(cmd);
    if (vr != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "cannot end the present commands (%d)", (int)vr);

    vr = d->da.vkResetFences(d->device, 1, &f->fence);
    if (vr != VK_SUCCESS) return aevk_fail(AEVK_ERR_OOM, "vkResetFences failed (%d)", (int)vr);

    /* The copy waits for the image at the transfer stage, the only stage
     * that touches it. */
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f->acquired;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &sc->rendered[idx];
    vr = d->da.vkQueueSubmit(d->queue, 1, &si, f->fence);
    if (vr != VK_SUCCESS) {
        return aevk_fail(vr == VK_ERROR_DEVICE_LOST ? AEVK_ERR_DEVICE_LOST : AEVK_ERR_OOM,
                         "vkQueueSubmit failed (%d)", (int)vr);
    }
    f->submitted = 1;
    sc->frame = (sc->frame + 1) % AEVK_PRESENT_FRAMES;

    VkPresentInfoKHR pi = {0};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &sc->rendered[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc->swapchain;
    pi.pImageIndices = &idx;
    vr = d->da.vkQueuePresentKHR(d->queue, &pi);
    if (vr == VK_SUCCESS || vr == VK_SUBOPTIMAL_KHR) {
        sc->presented++;
        if (vr == VK_SUBOPTIMAL_KHR) sc->stale = 1;
        return AEVK_OK;
    }
    if (vr == VK_ERROR_OUT_OF_DATE_KHR) {
        /* The window changed between acquire and present; this frame is
         * dropped and the next one rebuilds. */
        sc->stale = 1;
        return AEVK_OK;
    }
    if (vr == VK_ERROR_SURFACE_LOST_KHR) {
        return aevk_fail(AEVK_ERR_DEVICE_LOST, "the window's surface is gone");
    }
    return aevk_fail(vr == VK_ERROR_DEVICE_LOST ? AEVK_ERR_DEVICE_LOST : AEVK_ERR_OOM,
                     "vkQueuePresentKHR failed (%d)", (int)vr);
}

int aevk_present(AevkSwapchain* sc, AevkTarget* t) {
    aevk_clear_error();
    if (!sc || !t) return aevk_fail(AEVK_ERR_ARG, "swapchain or target is null");
    if (sc->dev != t->dev) return aevk_fail(AEVK_ERR_ARG, "the target belongs to another device");
    if (!t->rendered) {
        return aevk_fail(AEVK_ERR_ARG, "the target has no frame yet: draw or submit before presenting");
    }
    AevkDevice* d = sc->dev;
    AEVK_MUTEX_LOCK(&d->lock);
    int rc = aevk_present_locked(sc, t);
    AEVK_MUTEX_UNLOCK(&d->lock);
    return rc;
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

int aevk_ae_target_set_frames(void* t, int count) {
    return aevk_target_set_frames((AevkTarget*)t, count);
}
int aevk_ae_target_frames(void* t) { return aevk_target_frames((AevkTarget*)t); }
int aevk_ae_target_set_timeout_ms(void* t, int ms) {
    return aevk_target_set_timeout_ms((AevkTarget*)t, ms);
}
int aevk_ae_submit(void* t, void* p, double r, double g, double b, double a) {
    return aevk_submit((AevkTarget*)t, (AevkPipeline*)p,
                       (float)r, (float)g, (float)b, (float)a);
}
int aevk_ae_wait_all(void* t) { return aevk_wait_all((AevkTarget*)t); }

void* aevk_ae_target_create_ex(void* d, int w, int h, int want_depth, int samples) {
    return (void*)aevk_target_create_ex((AevkDevice*)d, w, h, want_depth, samples);
}
int aevk_ae_target_has_depth(void* t) { return aevk_target_has_depth((AevkTarget*)t); }
int aevk_ae_target_samples(void* t)   { return aevk_target_samples((AevkTarget*)t); }
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
    AevkMaterial* m = p->def;
    if (!m || !m->ub[binding].ptr) {
        return aevk_fail(AEVK_ERR_ARG, "call uniform_floats for binding %d first", binding);
    }
    int n = (int)(m->ub[binding].size / sizeof(float));
    if (index < 0 || index >= n) {
        return aevk_fail(AEVK_ERR_ARG, "uniform float %d is outside 0..%d", index, n - 1);
    }
    float f = (float)value;
    memcpy((char*)m->ub[binding].ptr + (size_t)index * sizeof(float), &f, sizeof(f));
    return AEVK_OK;
}

int aevk_ae_batch_reset(void* t) { return aevk_batch_reset((AevkTarget*)t); }
int aevk_ae_batch_add(void* t, void* m, int first, int count) {
    return aevk_batch_add((AevkTarget*)t, (AevkMaterial*)m, first, count);
}
int aevk_ae_batch_count(void* t) { return aevk_batch_count((const AevkTarget*)t); }

void* aevk_ae_material_create(void* pp) {
    return (void*)aevk_material_create((AevkPipeline*)pp);
}
void aevk_ae_material_destroy(void* mp) { aevk_material_destroy((AevkMaterial*)mp); }

int aevk_ae_material_set_texture(void* mp, int binding, void* tex) {
    return aevk_material_set_texture((AevkMaterial*)mp, binding, (AevkTexture*)tex);
}

int aevk_ae_material_set_uniform(void* mp, int binding, const void* data, int len) {
    if (len < 0) return aevk_fail(AEVK_ERR_ARG, "negative uniform length");
    return aevk_material_set_uniform((AevkMaterial*)mp, binding, data, (size_t)len);
}

int aevk_ae_material_floats(void* mp, int binding, int count) {
    AevkMaterial* m = (AevkMaterial*)mp;
    aevk_clear_error();
    if (!m) return aevk_fail(AEVK_ERR_ARG, "material is null");
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
    int rc = aevk_material_set_uniform(m, binding, zero, bytes);
    if (zero != stack) free(zero);
    return rc;
}

int aevk_ae_material_float(void* mp, int binding, int index, double value) {
    AevkMaterial* m = (AevkMaterial*)mp;
    aevk_clear_error();
    if (!m) return aevk_fail(AEVK_ERR_ARG, "material is null");
    if (binding < 0 || binding >= AEVK_MAX_DESC) {
        return aevk_fail(AEVK_ERR_ARG, "binding must be 0..%d", AEVK_MAX_DESC - 1);
    }
    if (!m->ub[binding].ptr) {
        return aevk_fail(AEVK_ERR_ARG, "call material_floats for binding %d first", binding);
    }
    int n = (int)(m->ub[binding].size / sizeof(float));
    if (index < 0 || index >= n) {
        return aevk_fail(AEVK_ERR_ARG, "uniform float %d is outside 0..%d", index, n - 1);
    }
    float f = (float)value;
    memcpy((char*)m->ub[binding].ptr + (size_t)index * sizeof(float), &f, sizeof(f));
    return AEVK_OK;
}

int aevk_ae_draw_material(void* t, void* p, void* m,
                          double r, double g, double b, double a) {
    return aevk_draw_material((AevkTarget*)t, (AevkPipeline*)p, (AevkMaterial*)m,
                              (float)r, (float)g, (float)b, (float)a);
}

int aevk_ae_submit_material(void* t, void* p, void* m,
                            double r, double g, double b, double a) {
    return aevk_submit_material((AevkTarget*)t, (AevkPipeline*)p, (AevkMaterial*)m,
                                (float)r, (float)g, (float)b, (float)a);
}

void* aevk_ae_texture_create_ex(void* d, int w, int h,
                                int mipmapped, int linear_filter, int repeat) {
    return (void*)aevk_texture_create_ex((AevkDevice*)d, w, h,
                                         mipmapped, linear_filter, repeat);
}

int aevk_ae_texture_mip_levels(void* tex) {
    return aevk_texture_mip_levels((const AevkTexture*)tex);
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
    if (t->vertex_count != count || t->vertex_floats != fpv) aevk_invalidate_records(t);
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
    aevk_invalidate_records(t);
    return AEVK_OK;
}

/* Index buffer. 32-bit indices only: the memory saved by 16-bit ones is not
 * worth a second code path at this size, and a caller who needs it can say
 * so when there is a reason. */
int aevk_ae_indices_reserve(void* tp, int count) {
    return aevk_ae_indices_reserve_ex(tp, count, 32);
}

/* `bits` is 16 or 32. Sixteen halves the index memory and the bandwidth to
 * read it, which is worth having for the meshes that fit: anything under
 * 65,536 vertices, i.e. most of them. */
int aevk_ae_indices_reserve_ex(void* tp, int count, int bits) {
    AevkTarget* t = (AevkTarget*)tp;
    aevk_clear_error();
    if (!t) return aevk_fail(AEVK_ERR_ARG, "target is null");
    if (count < 0) return aevk_fail(AEVK_ERR_ARG, "index count must not be negative");
    if (bits != 16 && bits != 32) {
        return aevk_fail(AEVK_ERR_ARG, "index width must be 16 or 32 bits (got %d)", bits);
    }
    size_t stride = (bits == 16) ? sizeof(uint16_t) : sizeof(uint32_t);
    if ((size_t)count > (size_t)(SIZE_MAX / stride)) {
        return aevk_fail(AEVK_ERR_ARG, "index count %d is too large", count);
    }

    AevkDevice* d = t->dev;
    int width_changed = (t->index_bits != bits);
    if (count > t->ibuf_capacity || width_changed) {
        VkDeviceSize need = (VkDeviceSize)count * (VkDeviceSize)stride;
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
    if (t->index_count != count || width_changed) aevk_invalidate_records(t);
    t->index_count = count;
    t->index_bits = bits;
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
    if (t->index_bits == 16) {
        if (value > 65535) {
            return aevk_fail(AEVK_ERR_ARG,
                             "vertex index %d does not fit in a 16-bit index", value);
        }
        ((uint16_t*)t->ibuf_ptr)[index] = (uint16_t)value;
    } else {
        ((uint32_t*)t->ibuf_ptr)[index] = (uint32_t)value;
    }
    aevk_invalidate_records(t);
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

/* The pixels of the frame that finished most recently, waiting for it first.
 * Every reader goes through this: with frames in flight the caller may not
 * have waited, and reading a buffer the GPU is still writing hands back a torn
 * frame. NULL when the target has no mapping or the wait failed. */
static const unsigned char* aevk_readable_pixels(AevkTarget* t) {
    if (!t || !t->frames || !t->readback_on) return NULL;
    AevkDevice* d = t->dev;
    AEVK_MUTEX_LOCK(&d->lock);
    /* The NEWEST frame, not the last one that happened to be waited on: a
     * caller reading after three submits means "show me what I just drew".
     * Earlier frames may still be in flight, and are waited on when their slot
     * comes round again or when the target is destroyed. */
    int slot = t->last_submitted >= 0 ? t->last_submitted : t->last_done;
    int rc = aevk_wait_frame_locked(t, slot);
    AEVK_MUTEX_UNLOCK(&d->lock);
    if (rc != AEVK_OK) return NULL;
    return (const unsigned char*)t->frames[slot].readback_ptr;
}

/* Packed 0xRRGGBBAA for one pixel, as a non-negative 64-bit value, or -1 when
 * the coordinates are outside the image or there is no frame to read. 64 bits
 * so that every colour, opaque white (0xFFFFFFFF) included, is distinct from
 * the failure. Reads the mapped buffer directly, so a test can sample without
 * copying the whole frame. */
int64_t aevk_ae_pixel(void* tp, int x, int y) {
    AevkTarget* t = (AevkTarget*)tp;
    aevk_clear_error();
    if (!t) { aevk_fail(AEVK_ERR_ARG, "target has no readback"); return -1; }
    if (x < 0 || y < 0 || x >= t->width || y >= t->height) {
        aevk_fail(AEVK_ERR_ARG, "pixel %d,%d is outside %dx%d", x, y, t->width, t->height);
        return -1;
    }
    if (!t->readback_on) {
        aevk_fail(AEVK_ERR_ARG, "readback is off for this target (target_set_readback)");
        return -1;
    }
    const unsigned char* base = aevk_readable_pixels(t);
    if (!base) { aevk_fail(AEVK_ERR_ARG, "target has no readback"); return -1; }
    const unsigned char* px =
        base + ((size_t)y * (size_t)t->width + (size_t)x) * (size_t)t->bytes_per_pixel;
    return (int64_t)(((uint32_t)aevk_channel_u8(t, px, 0) << 24) |
                     ((uint32_t)aevk_channel_u8(t, px, 1) << 16) |
                     ((uint32_t)aevk_channel_u8(t, px, 2) << 8)  |
                      (uint32_t)aevk_channel_u8(t, px, 3));
}

/* One channel of one pixel at full precision: the float value for the float
 * formats, the stored byte over 255 for the 8-bit ones. A float target's
 * HDR values above 1 are only visible this way. NaN (with the reason set)
 * for a bad coordinate or channel. */
double aevk_ae_pixel_value(void* tp, int x, int y, int channel) {
    AevkTarget* t = (AevkTarget*)tp;
    aevk_clear_error();
    const double nan = (double)NAN;
    if (!t) { aevk_fail(AEVK_ERR_ARG, "target is null"); return nan; }
    if (x < 0 || y < 0 || x >= t->width || y >= t->height) {
        aevk_fail(AEVK_ERR_ARG, "pixel %d,%d is outside %dx%d", x, y, t->width, t->height);
        return nan;
    }
    if (channel < 0 || channel > 3) {
        aevk_fail(AEVK_ERR_ARG, "channel %d is not 0..3", channel);
        return nan;
    }
    if (!t->readback_on) {
        aevk_fail(AEVK_ERR_ARG, "readback is off for this target (target_set_readback)");
        return nan;
    }
    const unsigned char* base = aevk_readable_pixels(t);
    if (!base) { aevk_fail(AEVK_ERR_ARG, "target has no readback"); return nan; }
    const unsigned char* px =
        base + ((size_t)y * (size_t)t->width + (size_t)x) * (size_t)t->bytes_per_pixel;
    return (double)aevk_channel_value(t, px, channel);
}

/* Binary PPM (P6). Chosen over PNG because it needs no compressor, so the
 * example and the CI leg carry no extra dependency and the bytes on disk are
 * trivially checkable. */
int aevk_ae_save_ppm(void* tp, const char* path) {
    AevkTarget* t = (AevkTarget*)tp;
    aevk_clear_error();
    if (!t || !path) return aevk_fail(AEVK_ERR_ARG, "target or path is null");
    if (!t->readback_on) {
        return aevk_fail(AEVK_ERR_ARG, "readback is off for this target (target_set_readback)");
    }
    const unsigned char* src = aevk_readable_pixels(t);
    if (!src) return aevk_fail(AEVK_ERR_ARG, "target has no readback");

    FILE* f = fopen(path, "wb");
    if (!f) return aevk_fail(AEVK_ERR_ARG, "cannot open %s for writing", path);
    if (fprintf(f, "P6\n%d %d\n255\n", t->width, t->height) < 0) {
        fclose(f);
        return aevk_fail(AEVK_ERR_ARG, "cannot write the PPM header to %s", path);
    }

    size_t pixels = (size_t)t->width * (size_t)t->height;
    for (size_t i = 0; i < pixels; i++) {
        const unsigned char* px = src + i * (size_t)t->bytes_per_pixel;
        unsigned char rgb[3] = { aevk_channel_u8(t, px, 0), aevk_channel_u8(t, px, 1),
                                 aevk_channel_u8(t, px, 2) };
        if (fwrite(rgb, 1, 3, f) != 3) {
            fclose(f);
            return aevk_fail(AEVK_ERR_ARG, "short write to %s", path);
        }
    }
    if (fclose(f) != 0) return aevk_fail(AEVK_ERR_ARG, "cannot flush %s", path);
    return AEVK_OK;
}

/* Presentation and resizing (#1505). `kind` is an AEVK_WINDOW_* value;
 * `display` and `window` are the handles a toolkit's native view hands out. */
int aevk_ae_target_resize(void* t, int w, int h) {
    return aevk_target_resize((AevkTarget*)t, w, h);
}
int aevk_ae_target_set_readback(void* t, int on) {
    return aevk_target_set_readback((AevkTarget*)t, on);
}
int aevk_ae_target_readback(void* t) { return aevk_target_readback((const AevkTarget*)t); }

/* Colour formats (#1514). */
void* aevk_ae_target_create_format(void* d, int w, int h, int format, int want_depth, int samples) {
    return (void*)aevk_target_create_format((AevkDevice*)d, w, h, format, want_depth, samples);
}
int aevk_ae_target_format(void* t) { return aevk_target_format((const AevkTarget*)t); }
int aevk_ae_target_bytes_per_pixel(void* t) {
    return aevk_target_bytes_per_pixel((const AevkTarget*)t);
}
int aevk_ae_copy_rgba8(void* t, void* dest, int dest_len) {
    if (dest_len < 0) return aevk_fail(AEVK_ERR_ARG, "negative destination length");
    return aevk_read_rgba8((AevkTarget*)t, dest, (size_t)dest_len);
}

void* aevk_ae_swapchain_create(void* d, int kind, void* display, void* window, int w, int h) {
    return (void*)aevk_swapchain_create((AevkDevice*)d, kind, display, window, w, h);
}
void aevk_ae_swapchain_destroy(void* sc) { aevk_swapchain_destroy((AevkSwapchain*)sc); }
int aevk_ae_swapchain_resize(void* sc, int w, int h) {
    return aevk_swapchain_resize((AevkSwapchain*)sc, w, h);
}
int aevk_ae_swapchain_set_vsync(void* sc, int on) {
    return aevk_swapchain_set_vsync((AevkSwapchain*)sc, on);
}
int aevk_ae_swapchain_width(void* sc)  { return aevk_swapchain_width((const AevkSwapchain*)sc); }
int aevk_ae_swapchain_height(void* sc) { return aevk_swapchain_height((const AevkSwapchain*)sc); }
int aevk_ae_swapchain_format(void* sc) { return aevk_swapchain_format((const AevkSwapchain*)sc); }
/* The count as an int: two billion presented frames is over a year at 60 Hz,
 * and an Aether int is what the rest of the surface speaks. */
int aevk_ae_swapchain_presented(void* sc) {
    long long n = aevk_swapchain_presented((const AevkSwapchain*)sc);
    return n > 0x7fffffffLL ? 0x7fffffff : (int)n;
}
int aevk_ae_present(void* sc, void* t) {
    return aevk_present((AevkSwapchain*)sc, (AevkTarget*)t);
}

/* Buffers and compute (#1515). */
void* aevk_ae_buffer_create(void* d, int bytes) {
    if (bytes < 0) { aevk_fail(AEVK_ERR_ARG, "negative buffer size"); return NULL; }
    return (void*)aevk_buffer_create((AevkDevice*)d, (size_t)bytes);
}
void aevk_ae_buffer_destroy(void* b) { aevk_buffer_destroy((AevkBuffer*)b); }
int aevk_ae_buffer_size(void* b) {
    size_t n = aevk_buffer_size((const AevkBuffer*)b);
    return n > 0x7fffffffu ? 0x7fffffff : (int)n;
}
int aevk_ae_buffer_write(void* b, int offset, const void* data, int len) {
    if (offset < 0 || len < 0) return aevk_fail(AEVK_ERR_ARG, "negative offset or length");
    return aevk_buffer_write((AevkBuffer*)b, (size_t)offset, data, (size_t)len);
}
int aevk_ae_buffer_read(void* b, int offset, void* dest, int len) {
    if (offset < 0 || len < 0) return aevk_fail(AEVK_ERR_ARG, "negative offset or length");
    return aevk_buffer_read((AevkBuffer*)b, (size_t)offset, dest, (size_t)len);
}
/* Element access by 4-byte index, so a caller fills a float[] or int[] the
 * shader declares without packing bytes. */
int aevk_ae_buffer_set_float(void* b, int index, double value) {
    float f = (float)value;
    if (index < 0) return aevk_fail(AEVK_ERR_ARG, "negative index");
    return aevk_buffer_write((AevkBuffer*)b, (size_t)index * 4u, &f, sizeof(f));
}
double aevk_ae_buffer_float(void* b, int index) {
    float f = 0.0f;
    if (index < 0) { aevk_fail(AEVK_ERR_ARG, "negative index"); return (double)NAN; }
    if (aevk_buffer_read((AevkBuffer*)b, (size_t)index * 4u, &f, sizeof(f)) != AEVK_OK) {
        return (double)NAN;
    }
    return (double)f;
}
int aevk_ae_buffer_set_int(void* b, int index, int value) {
    int32_t v = (int32_t)value;
    if (index < 0) return aevk_fail(AEVK_ERR_ARG, "negative index");
    return aevk_buffer_write((AevkBuffer*)b, (size_t)index * 4u, &v, sizeof(v));
}
/* 0 with the reason set when the index is outside the buffer; check
 * last_error() when 0 is a value the buffer could hold. */
int aevk_ae_buffer_int(void* b, int index) {
    int32_t v = 0;
    if (index < 0) { aevk_fail(AEVK_ERR_ARG, "negative index"); return 0; }
    if (aevk_buffer_read((AevkBuffer*)b, (size_t)index * 4u, &v, sizeof(v)) != AEVK_OK) return 0;
    return (int)v;
}

int aevk_ae_bindings_storage(void* b, int binding) {
    return aevk_bindings_storage((AevkBindings*)b, binding);
}
int aevk_ae_material_set_buffer(void* m, int binding, void* buf) {
    return aevk_material_set_buffer((AevkMaterial*)m, binding, (AevkBuffer*)buf);
}
int aevk_ae_set_buffer(void* p, int binding, void* buf) {
    return aevk_pipeline_set_buffer((AevkPipeline*)p, binding, (AevkBuffer*)buf);
}

void* aevk_ae_compute_create(void* d, const char* spv, int len, void* bindings, int push_bytes) {
    if (len < 0) { aevk_fail(AEVK_ERR_SHADER, "negative SPIR-V length"); return NULL; }
    return (void*)aevk_compute_create((AevkDevice*)d, spv, (size_t)len,
                                      (const AevkBindings*)bindings, push_bytes);
}
void aevk_ae_compute_destroy(void* c) { aevk_compute_destroy((AevkCompute*)c); }
int aevk_ae_compute_set_buffer(void* c, int binding, void* buf) {
    return aevk_compute_set_buffer((AevkCompute*)c, binding, (AevkBuffer*)buf);
}
int aevk_ae_compute_set_texture(void* c, int binding, void* tex) {
    return aevk_compute_set_texture((AevkCompute*)c, binding, (AevkTexture*)tex);
}
int aevk_ae_compute_set_push(void* c, const void* data, int len) {
    if (len < 0) return aevk_fail(AEVK_ERR_ARG, "negative push length");
    return aevk_compute_set_push((AevkCompute*)c, data, (size_t)len);
}
/* Push constants by 4-byte slot: a float, or a 32-bit int (an `int` or
 * `uint` member in the shader's block). */
static int aevk_compute_push_word(AevkCompute* c, int index, const void* word) {
    aevk_clear_error();
    if (!c) return aevk_fail(AEVK_ERR_ARG, "compute is null");
    int n = (int)(c->push_bytes / 4u);
    if (index < 0 || index >= n) {
        return aevk_fail(AEVK_ERR_ARG, "push slot %d is outside 0..%d", index, n - 1);
    }
    memcpy(c->push + (size_t)index * 4u, word, 4);
    return AEVK_OK;
}
int aevk_ae_compute_push_float(void* c, int index, double value) {
    float f = (float)value;
    return aevk_compute_push_word((AevkCompute*)c, index, &f);
}
int aevk_ae_compute_push_int(void* c, int index, int value) {
    int32_t v = (int32_t)value;
    return aevk_compute_push_word((AevkCompute*)c, index, &v);
}
int aevk_ae_compute_set_timeout_ms(void* c, int ms) {
    return aevk_compute_set_timeout_ms((AevkCompute*)c, ms);
}
int aevk_ae_dispatch(void* c, int gx, int gy, int gz) {
    return aevk_dispatch((AevkCompute*)c, gx, gy, gz);
}
int aevk_ae_dispatch_async(void* c, int gx, int gy, int gz) {
    return aevk_dispatch_async((AevkCompute*)c, gx, gy, gz);
}
int aevk_ae_compute_wait(void* c) { return aevk_compute_wait((AevkCompute*)c); }

/* The loader's entry points for contrib.vulkan.vk, the generated module that
 * drives the API directly (#1506). It goes through the loader this file
 * opens at runtime rather than linking one, so a program using it starts
 * where there is no Vulkan and learns so from the results, as one using the
 * rest of the module does. Each returns NULL where there is no loader or no
 * such entry point. */

/* An entry point the loader exports: every core command and the window-
 * system ones (surface, swapchain), each dispatching on its first handle. */
void* aevk_ae_loader_proc(const char* name) {
    if (!name || aevk_load_library() != AEVK_OK) return NULL;
    return (void*)AEVK_DLSYM(g_lib, name);
}

/* vkGetInstanceProcAddr: `instance` may be NULL for the global commands. */
void* aevk_ae_instance_proc(void* instance, const char* name) {
    if (!name || aevk_load_library() != AEVK_OK) return NULL;
    return (void*)g_gipa((VkInstance)instance, name);
}

/* vkGetDeviceProcAddr: the driver's own entry, skipping the loader's
 * dispatch, for a command whose first handle belongs to `device`. */
void* aevk_ae_device_proc(void* device, const char* name) {
    if (!name || !device || aevk_load_library() != AEVK_OK) return NULL;
    PFN_vkGetDeviceProcAddr gdpa =
        (PFN_vkGetDeviceProcAddr)AEVK_DLSYM(g_lib, "vkGetDeviceProcAddr");
    if (!gdpa) return NULL;
    return (void*)gdpa((VkDevice)device, name);
}

/* The arrays contrib.vulkan.vk's <command>_all helpers fill: zeroed, and
 * NULL when count * size overflows or memory runs out. */
void* aevk_ae_array_alloc(int count, int size) {
    if (count <= 0 || size <= 0 || (size_t)count > SIZE_MAX / (size_t)size) return NULL;
    return calloc((size_t)count, (size_t)size);
}

void aevk_ae_array_free(void* p) { free(p); }

/* A string argument for a command contrib.vulkan.vk calls through a function
 * pointer: the C characters of whatever Aether holds (a wrapped AetherString
 * included), with NULL kept NULL, since Vulkan tells a NULL layer name (the
 * loader's own extensions) from an empty one. Declared `@aether string` on
 * the Aether side, so it receives the value as Aether holds it. */
extern const char* aether_string_data(const void* s);
void* aevk_ae_cstr(const void* s) {
    return s ? (void*)aether_string_data(s) : NULL;
}

/* sizeof a handle, which Aether cannot spell: a dispatchable one is a
 * pointer, a non-dispatchable one a pointer on 64-bit targets and a
 * uint64_t on 32-bit ones. */
int aevk_ae_handle_size(int dispatchable) {
    return dispatchable ? (int)sizeof(VkInstance) : (int)sizeof(VkSemaphore);
}
