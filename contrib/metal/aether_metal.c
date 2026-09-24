/* contrib/metal: GPU rendering, compute and presentation with Metal.
 * See aether_metal.h for the API, the argument-table mapping and the
 * conventions.
 */

#include "aether_metal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#  define AEMT_THREAD_LOCAL __thread
#else
#  define AEMT_THREAD_LOCAL
#endif

static AEMT_THREAD_LOCAL char g_err[512];

static int aemt_fail(int code, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
    return code;
}

const char* aemt_last_error(void) { return g_err; }

#if defined(__APPLE__)

static void aemt_clear_error(void) { g_err[0] = '\0'; }

/* The bytes a pixel of each target format takes, 0 for anything else. */
static int aemt_format_bpp(int f) {
    switch (f) {
        case AEMT_FORMAT_R8G8B8A8_UNORM:
        case AEMT_FORMAT_R8G8B8A8_SRGB:       return 4;
        case AEMT_FORMAT_R16G16B16A16_SFLOAT: return 8;
        case AEMT_FORMAT_R32G32B32A32_SFLOAT: return 16;
        default:                              return 0;
    }
}

/* Everything goes through the Objective-C runtime, so this file is C and the
 * link line names no framework: libobjc, Foundation, QuartzCore and Metal are
 * opened the way contrib/vulkan opens the Vulkan loader. What crosses
 * objc_msgSend by value is laid out below as the 64-bit ABI lays it out
 * (NSUInteger is unsigned long, CGFloat double), and the values come from
 * Apple's metal-cpp headers. */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include <pthread.h>

typedef unsigned long AemtUInt;
typedef struct { double red, green, blue, alpha; } AemtClearColor;
typedef struct { double x, y, width, height, znear, zfar; } AemtViewport;
typedef struct { AemtUInt x, y, z; } AemtOrigin;
typedef struct { AemtUInt width, height, depth; } AemtSize;
typedef struct { double width, height; } AemtCGSize;
typedef struct { double x, y, width, height; } AemtCGRect;

enum {
    MTL_PIXEL_RGBA8_UNORM      = 70,
    MTL_PIXEL_RGBA8_UNORM_SRGB = 71,
    MTL_PIXEL_BGRA8_UNORM      = 80,
    MTL_PIXEL_BGRA8_UNORM_SRGB = 81,
    MTL_PIXEL_RGBA16_FLOAT     = 115,
    MTL_PIXEL_RGBA32_FLOAT     = 125,
    MTL_PIXEL_DEPTH32_FLOAT    = 252,

    MTL_VERTEX_UCHAR4_NORMALIZED = 9,
    MTL_VERTEX_FLOAT             = 28,
    MTL_VERTEX_FLOAT2            = 29,
    MTL_VERTEX_FLOAT3            = 30,
    MTL_VERTEX_FLOAT4            = 31,
    MTL_STEP_PER_VERTEX          = 1,
    MTL_STEP_PER_INSTANCE        = 2,

    MTL_LOAD_DONT_CARE         = 0,
    MTL_LOAD_CLEAR             = 2,
    MTL_STORE_DONT_CARE        = 0,
    MTL_STORE_STORE            = 1,
    MTL_STORE_RESOLVE          = 2,

    MTL_STORAGE_PRIVATE        = 2,
    MTL_RESOURCE_SHARED        = 0x0,
    MTL_TEXTURE_2D             = 2,
    MTL_TEXTURE_2D_MULTISAMPLE = 4,
    MTL_USAGE_SHADER_READ      = 1,
    MTL_USAGE_RENDER_TARGET    = 4,

    MTL_PRIMITIVE_TRIANGLE     = 3,
    MTL_INDEX_UINT16           = 0,
    MTL_INDEX_UINT32           = 1,
    MTL_COMPARE_LESS           = 1,

    MTL_FILTER_NEAREST         = 0,
    MTL_FILTER_LINEAR          = 1,
    MTL_MIP_NOT_MIPMAPPED      = 0,
    MTL_MIP_NEAREST            = 1,
    MTL_MIP_LINEAR             = 2,
    MTL_ADDRESS_CLAMP_TO_EDGE  = 0,
    MTL_ADDRESS_REPEAT         = 2,

    MTL_FUNCTION_VERTEX        = 1,
    MTL_FUNCTION_FRAGMENT      = 2,
    MTL_FUNCTION_KERNEL        = 3,

    MTL_CB_COMPLETED           = 4,
    MTL_CB_ERROR               = 5,

    NS_UTF8_STRING_ENCODING    = 4
};

/* Every Metal GPU on macOS supports 2D textures this large (the Mac2 and
 * Apple3 and later families of Apple's Metal feature set tables). */
#define AEMT_MAX_TEXTURE 16384

/* ------------------------------------------------------------------------ */
/* Runtime loading                                                           */
/* ------------------------------------------------------------------------ */

static struct {
    int    loaded;              /* 0 unprobed, 1 ok, -1 not */
    void*  objc;
    void*  foundation;
    void*  quartz;
    void*  metal;
    Class  (*get_class)(const char*);
    SEL    (*sel)(const char*);
    void*  send;
    void*  send_stret;          /* struct returns above 16 bytes on x86_64 */
    void*  (*pool_push)(void);
    void   (*pool_pop)(void*);
    id     (*create_device)(void);
} g_mt;

static pthread_mutex_t g_load_lock = PTHREAD_MUTEX_INITIALIZER;

#define MT_SEND(ret, ...) ((ret (*)(id, SEL, ##__VA_ARGS__))g_mt.send)

static id  mt_cls(const char* name) { return (id)g_mt.get_class(name); }
static SEL mt_sel(const char* name) { return g_mt.sel(name); }

static int aemt_load(void) {
    pthread_mutex_lock(&g_load_lock);
    if (g_mt.loaded) {
        int ok = g_mt.loaded > 0;
        pthread_mutex_unlock(&g_load_lock);
        return ok ? AEMT_OK : aemt_fail(AEMT_ERR_NO_LOADER, "Metal is not available on this system");
    }
    g_mt.loaded = -1;
    int rc = AEMT_OK;
    g_mt.objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    g_mt.foundation = dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW | RTLD_GLOBAL);
    g_mt.quartz = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW | RTLD_GLOBAL);
    g_mt.metal = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_GLOBAL);
    if (!g_mt.objc || !g_mt.foundation || !g_mt.quartz || !g_mt.metal) {
        rc = aemt_fail(AEMT_ERR_NO_LOADER, "cannot load the Metal framework: %s", dlerror());
    } else {
        g_mt.get_class = (Class (*)(const char*))dlsym(g_mt.objc, "objc_getClass");
        g_mt.sel = (SEL (*)(const char*))dlsym(g_mt.objc, "sel_registerName");
        g_mt.send = dlsym(g_mt.objc, "objc_msgSend");
#if defined(__x86_64__)
        g_mt.send_stret = dlsym(g_mt.objc, "objc_msgSend_stret");
#endif
        g_mt.pool_push = (void* (*)(void))dlsym(g_mt.objc, "objc_autoreleasePoolPush");
        g_mt.pool_pop = (void (*)(void*))dlsym(g_mt.objc, "objc_autoreleasePoolPop");
        g_mt.create_device = (id (*)(void))dlsym(g_mt.metal, "MTLCreateSystemDefaultDevice");
        if (!g_mt.get_class || !g_mt.sel || !g_mt.send || !g_mt.pool_push || !g_mt.pool_pop ||
            !g_mt.create_device
#if defined(__x86_64__)
            || !g_mt.send_stret
#endif
            ) {
            rc = aemt_fail(AEMT_ERR_NO_LOADER, "the Objective-C runtime or Metal is missing an entry point");
        } else {
            g_mt.loaded = 1;
        }
    }
    pthread_mutex_unlock(&g_load_lock);
    return rc;
}

/* Objects made by alloc/new/copy or a Create function are owned (+1) and
 * released here; everything else is autoreleased, so every entry point that
 * talks to Metal runs inside its own autorelease pool and retains what it
 * keeps. */
static void mt_release(id o) {
    if (o) MT_SEND(void)(o, mt_sel("release"));
}

static id mt_retain(id o) {
    return o ? MT_SEND(id)(o, mt_sel("retain")) : NULL;
}

static id mt_new(const char* cls) {
    return MT_SEND(id)(mt_cls(cls), mt_sel("new"));
}

static const char* mt_utf8(id str) {
    return str ? MT_SEND(const char*)(str, mt_sel("UTF8String")) : "";
}

/* An NSError's description, for last_error(). */
static const char* mt_error_text(id error) {
    if (!error) return "no reason given";
    return mt_utf8(MT_SEND(id)(error, mt_sel("localizedDescription")));
}

/* Whether `obj` implements `selector`: for the properties newer than the
 * oldest macOS this runs on, which an older system would answer with an
 * unrecognised-selector exception. */
static int mt_responds(id obj, const char* selector) {
    return obj && MT_SEND(BOOL, SEL)(obj, mt_sel("respondsToSelector:"), mt_sel(selector)) ? 1 : 0;
}

/* A BOOL property that may not exist on this system; 0 when it does not. */
static int mt_flag(id obj, const char* selector) {
    return mt_responds(obj, selector) && MT_SEND(BOOL)(obj, mt_sel(selector)) ? 1 : 0;
}

static id mt_at(id array, AemtUInt i) {
    return MT_SEND(id, AemtUInt)(array, mt_sel("objectAtIndexedSubscript:"), i);
}

/* A method returning a CGRect: 32 bytes come back through objc_msgSend_stret
 * on x86_64; arm64 returns them through objc_msgSend. */
static AemtCGRect mt_rect(id obj, const char* selector) {
#if defined(__x86_64__)
    return ((AemtCGRect (*)(id, SEL))g_mt.send_stret)(obj, mt_sel(selector));
#else
    return MT_SEND(AemtCGRect)(obj, mt_sel(selector));
#endif
}

/* ------------------------------------------------------------------------ */
/* Objects                                                                   */
/* ------------------------------------------------------------------------ */

#define AEMT_MAX_BINDINGS 8          /* vertex streams */
#define AEMT_MAX_ATTRS    16
#define AEMT_MAX_DESC     8          /* bindings: buffer / texture indices 0..7 */
#define AEMT_MAX_PUSH     128
#define AEMT_MAX_FRAMES   8
#define AEMT_PUSH_INDEX   8          /* [[buffer(8)]] */
#define AEMT_STREAM_BASE  16         /* vertex stream B is [[buffer(16 + B)]] */

struct AemtDevice {
    pthread_mutex_t lock;
    id              device;
    id              queue;
    int             unified;
    int             msaa32;          /* resolves multisampled 32-bit float */
    int             filter32;        /* filters 32-bit float textures linearly */
    /* The present pass (a textured full-screen triangle), made on first use:
     * [0] writes a UNORM drawable, [1] an sRGB one. The nearest sampler is
     * for a 32-bit float target on a device that cannot filter one. */
    id              present_pipe[2];
    id              present_sampler;
    id              present_sampler_nearest;
    char            name[256];
};

/* ------------------------------------------------------------------------ */
/* Device                                                                    */
/* ------------------------------------------------------------------------ */

static char g_probe_name[256];
static int  g_probe;   /* 0 unprobed, 1 usable, -1 not */
static pthread_mutex_t g_probe_lock = PTHREAD_MUTEX_INITIALIZER;

int aemt_available(void) {
    pthread_mutex_lock(&g_probe_lock);
    if (!g_probe) {
        g_probe = -1;
        if (aemt_load() == AEMT_OK) {
            void* pool = g_mt.pool_push();
            id dev = g_mt.create_device();
            if (dev) {
                snprintf(g_probe_name, sizeof(g_probe_name), "%s", mt_utf8(MT_SEND(id)(dev, mt_sel("name"))));
                mt_release(dev);
                g_probe = 1;
                aemt_clear_error();
            } else {
                aemt_fail(AEMT_ERR_NO_DEVICE, "MTLCreateSystemDefaultDevice returned no device");
            }
            g_mt.pool_pop(pool);
        }
    }
    int ok = g_probe > 0;
    pthread_mutex_unlock(&g_probe_lock);
    return ok;
}

const char* aemt_device_name(void) {
    return aemt_available() ? g_probe_name : "";
}

AemtDevice* aemt_device_create(void) {
    aemt_clear_error();
    if (aemt_load() != AEMT_OK) return NULL;
    AemtDevice* d = (AemtDevice*)calloc(1, sizeof(*d));
    if (!d) { aemt_fail(AEMT_ERR_OOM, "out of memory"); return NULL; }
    pthread_mutex_init(&d->lock, NULL);
    void* pool = g_mt.pool_push();
    d->device = g_mt.create_device();
    if (!d->device) {
        aemt_fail(AEMT_ERR_NO_DEVICE, "MTLCreateSystemDefaultDevice returned no device");
        goto fail;
    }
    d->queue = MT_SEND(id)(d->device, mt_sel("newCommandQueue"));
    if (!d->queue) { aemt_fail(AEMT_ERR_OOM, "newCommandQueue failed"); goto fail; }
    d->unified = MT_SEND(BOOL)(d->device, mt_sel("hasUnifiedMemory")) ? 1 : 0;
    /* Both answer for 32-bit float formats, which not every GPU resolves or
     * filters. */
    d->msaa32 = mt_flag(d->device, "supports32BitMSAA");
    d->filter32 = mt_flag(d->device, "supports32BitFloatFiltering");
    snprintf(d->name, sizeof(d->name), "%s", mt_utf8(MT_SEND(id)(d->device, mt_sel("name"))));
    g_mt.pool_pop(pool);
    return d;
fail:
    g_mt.pool_pop(pool);
    aemt_device_destroy(d);
    return NULL;
}

int aemt_device_unified_memory(const AemtDevice* d) { return d ? d->unified : 0; }

/* Commits an empty command buffer and waits for it: every command buffer
 * committed to the queue before it has completed when it has. */
static void aemt_idle(AemtDevice* d) {
    void* pool = g_mt.pool_push();
    id cb = MT_SEND(id)(d->queue, mt_sel("commandBuffer"));
    if (cb) {
        MT_SEND(void)(cb, mt_sel("commit"));
        MT_SEND(void)(cb, mt_sel("waitUntilCompleted"));
    }
    g_mt.pool_pop(pool);
}

void aemt_device_destroy(AemtDevice* d) {
    if (!d) return;
    if (d->queue) aemt_idle(d);
    for (int i = 0; i < 2; i++) mt_release(d->present_pipe[i]);
    mt_release(d->present_sampler);
    mt_release(d->present_sampler_nearest);
    mt_release(d->queue);
    mt_release(d->device);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

/* A command buffer's outcome once it has completed: AEMT_OK, or the error it
 * carries. */
static int aemt_cb_status(id cb) {
    AemtUInt status = MT_SEND(AemtUInt)(cb, mt_sel("status"));
    if (status == MTL_CB_ERROR) {
        return aemt_fail(AEMT_ERR_DEVICE_LOST, "the GPU reported an error: %s",
                         mt_error_text(MT_SEND(id)(cb, mt_sel("error"))));
    }
    return AEMT_OK;
}

/* A committed command buffer and the semaphore its completion handler
 * signals, so a wait can give up after a timeout instead of blocking for as
 * long as the GPU takes. */
typedef struct {
    id                   cb;
    dispatch_semaphore_t done;
    int                  submitted;
} AemtFence;

static int aemt_fence_init(AemtFence* f) {
    memset(f, 0, sizeof(*f));
    f->done = dispatch_semaphore_create(0);
    return f->done ? AEMT_OK : aemt_fail(AEMT_ERR_OOM, "dispatch_semaphore_create failed");
}

/* Commits `cb` (retained here) with a handler that signals the fence. The
 * fence must be idle. */
static void aemt_fence_commit(AemtFence* f, id cb) {
    dispatch_semaphore_t done = f->done;
    MT_SEND(void, void*)(cb, mt_sel("addCompletedHandler:"), (void*)^(id completed) {
        (void)completed;
        dispatch_semaphore_signal(done);
    });
    f->cb = mt_retain(cb);
    f->submitted = 1;
    MT_SEND(void)(cb, mt_sel("commit"));
}

static int aemt_fence_wait(AemtFence* f, int timeout_ms) {
    if (!f->submitted) return AEMT_OK;
    dispatch_time_t until = dispatch_time(DISPATCH_TIME_NOW, (int64_t)timeout_ms * 1000000LL);
    if (dispatch_semaphore_wait(f->done, until) != 0) {
        /* Still running: the handler signals later, and the next wait on
         * this fence takes that signal. */
        return aemt_fail(AEMT_ERR_DEVICE_LOST, "the GPU did not finish within %d ms", timeout_ms);
    }
    /* Its own pool: the error text and the command buffer's release may
     * autorelease, and the waits run from calls that hold none. */
    void* pool = g_mt.pool_push();
    int rc = aemt_cb_status(f->cb);
    mt_release(f->cb);
    g_mt.pool_pop(pool);
    f->cb = NULL;
    f->submitted = 0;
    return rc;
}

/* Waits however long it takes: before the semaphore is released, since the
 * handler of a command buffer still running would signal freed memory. */
static void aemt_fence_free(AemtFence* f) {
    if (f->submitted) {
        dispatch_semaphore_wait(f->done, DISPATCH_TIME_FOREVER);
        mt_release(f->cb);
    }
    if (f->done) dispatch_release(f->done);
    memset(f, 0, sizeof(*f));
}

/* A shared buffer of `bytes`, zeroed; its contents pointer in *ptr. */
static id aemt_make_buffer(AemtDevice* d, size_t bytes, unsigned char** ptr) {
    id b = MT_SEND(id, AemtUInt, AemtUInt)(d->device, mt_sel("newBufferWithLength:options:"),
                                           (AemtUInt)bytes, (AemtUInt)MTL_RESOURCE_SHARED);
    if (!b) {
        aemt_fail(AEMT_ERR_OOM, "newBufferWithLength (%zu bytes) failed", bytes);
        return NULL;
    }
    *ptr = (unsigned char*)MT_SEND(void*)(b, mt_sel("contents"));
    memset(*ptr, 0, bytes);
    return b;
}

/* A private 2D texture: `samples` above 1 makes it multisampled. */
static id aemt_make_texture(AemtDevice* d, int w, int h, AemtUInt pixel_format, int samples,
                            int mips, AemtUInt usage) {
    id desc = mt_new("MTLTextureDescriptor");
    if (!desc) { aemt_fail(AEMT_ERR_OOM, "MTLTextureDescriptor is missing"); return NULL; }
    MT_SEND(void, AemtUInt)(desc, mt_sel("setTextureType:"),
                            samples > 1 ? (AemtUInt)MTL_TEXTURE_2D_MULTISAMPLE : (AemtUInt)MTL_TEXTURE_2D);
    MT_SEND(void, AemtUInt)(desc, mt_sel("setPixelFormat:"), pixel_format);
    MT_SEND(void, AemtUInt)(desc, mt_sel("setWidth:"), (AemtUInt)w);
    MT_SEND(void, AemtUInt)(desc, mt_sel("setHeight:"), (AemtUInt)h);
    MT_SEND(void, AemtUInt)(desc, mt_sel("setMipmapLevelCount:"), (AemtUInt)mips);
    MT_SEND(void, AemtUInt)(desc, mt_sel("setSampleCount:"), (AemtUInt)samples);
    MT_SEND(void, AemtUInt)(desc, mt_sel("setUsage:"), usage);
    MT_SEND(void, AemtUInt)(desc, mt_sel("setStorageMode:"), (AemtUInt)MTL_STORAGE_PRIVATE);
    id t = MT_SEND(id, id)(d->device, mt_sel("newTextureWithDescriptor:"), desc);
    mt_release(desc);
    if (!t) aemt_fail(AEMT_ERR_OOM, "newTextureWithDescriptor (%dx%d, format %lu) failed", w, h, pixel_format);
    return t;
}

/* ------------------------------------------------------------------------ */
/* Shaders                                                                   */
/* ------------------------------------------------------------------------ */

/* A library from MSL source, or from a metallib (recognised by its MTLB
 * magic). Owned. */
static id aemt_library(AemtDevice* d, const void* src, size_t len, const char* stage) {
    if (!src || len == 0) {
        aemt_fail(AEMT_ERR_SHADER, "the %s shader is empty", stage);
        return NULL;
    }
    id error = NULL;
    id lib = NULL;
    if (len >= 4 && memcmp(src, "MTLB", 4) == 0) {
        /* Copied by dispatch_data_create, so the caller's bytes are free to
         * go once this returns. */
        dispatch_data_t data = dispatch_data_create(src, len, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        if (!data) { aemt_fail(AEMT_ERR_OOM, "out of memory"); return NULL; }
        lib = MT_SEND(id, void*, id*)(d->device, mt_sel("newLibraryWithData:error:"), (void*)data, &error);
        dispatch_release(data);
        if (!lib) aemt_fail(AEMT_ERR_SHADER, "the %s metallib was rejected: %s", stage, mt_error_text(error));
        return lib;
    }
    id text = MT_SEND(id)(mt_cls("NSString"), mt_sel("alloc"));
    text = MT_SEND(id, const void*, AemtUInt, AemtUInt)(text, mt_sel("initWithBytes:length:encoding:"),
                                                        src, (AemtUInt)len, (AemtUInt)NS_UTF8_STRING_ENCODING);
    if (!text) {
        aemt_fail(AEMT_ERR_SHADER, "the %s shader is neither UTF-8 MSL source nor a metallib", stage);
        return NULL;
    }
    id opts = mt_new("MTLCompileOptions");
    lib = MT_SEND(id, id, id, id*)(d->device, mt_sel("newLibraryWithSource:options:error:"), text, opts, &error);
    mt_release(opts);
    mt_release(text);
    if (!lib) aemt_fail(AEMT_ERR_SHADER, "%s: %s", stage, mt_error_text(error));
    return lib;
}

/* The library's one function of `type`, whatever it is called: MSL reserves
 * no entry-point name, so the stage is what identifies it. Owned. */
static id aemt_function(id lib, AemtUInt type, const char* stage) {
    id names = MT_SEND(id)(lib, mt_sel("functionNames"));
    AemtUInt n = names ? MT_SEND(AemtUInt)(names, mt_sel("count")) : 0;
    id found = NULL;
    int count = 0;
    char seen[256] = "";
    for (AemtUInt i = 0; i < n; i++) {
        id name = mt_at(names, i);
        id fn = MT_SEND(id, id)(lib, mt_sel("newFunctionWithName:"), name);
        if (!fn) continue;
        if (MT_SEND(AemtUInt)(fn, mt_sel("functionType")) == type) {
            count++;
            size_t used = strlen(seen);
            snprintf(seen + used, sizeof(seen) - used, "%s%s", used ? ", " : "", mt_utf8(name));
            if (!found) { found = fn; continue; }
        }
        mt_release(fn);
    }
    if (count == 1) return found;
    mt_release(found);
    if (count == 0) aemt_fail(AEMT_ERR_SHADER, "the %s shader has no %s function", stage, stage);
    else aemt_fail(AEMT_ERR_SHADER, "the %s shader has %d %s functions (%s); it needs exactly one",
                   stage, count, stage, seen);
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Targets                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    AemtMaterial* mat;
    int           first;
    int           count;
} AemtDrawItem;

typedef struct {
    AemtFence      fence;
    id             readback;       /* shared buffer, w * h * bpp, tightly packed */
    unsigned char* readback_ptr;
} AemtFrame;

struct AemtTarget {
    AemtDevice*    dev;
    int            width, height;
    int            format;         /* AEMT_FORMAT_* */
    AemtUInt       pixel_format;   /* its MTLPixelFormat */
    int            bpp;
    int            samples;
    int            has_depth;
    id             color;          /* single-sample: resolved into, read back, presented */
    id             msaa;
    id             depth;
    int            readback_on;
    int            rendered;

    id             vbuf;
    unsigned char* vbuf_ptr;
    int            vbuf_capacity;  /* bytes */
    int            vertex_count;
    int            vertex_floats;
    id             ibuf;
    unsigned char* ibuf_ptr;
    int            ibuf_capacity;  /* bytes */
    int            index_count;
    int            index_bits;

    AemtDrawItem*  batch;
    int            batch_count, batch_cap;
    unsigned char  push[AEMT_MAX_PUSH];
    int            push_size;

    AemtFrame      frames[AEMT_MAX_FRAMES];
    int            frame_count;
    int            next_frame;
    int            last_submitted;
    int            timeout_ms;
};

static AemtUInt aemt_pixel_format(int f) {
    switch (f) {
        case AEMT_FORMAT_R8G8B8A8_UNORM:      return MTL_PIXEL_RGBA8_UNORM;
        case AEMT_FORMAT_R8G8B8A8_SRGB:       return MTL_PIXEL_RGBA8_UNORM_SRGB;
        case AEMT_FORMAT_R16G16B16A16_SFLOAT: return MTL_PIXEL_RGBA16_FLOAT;
        case AEMT_FORMAT_R32G32B32A32_SFLOAT: return MTL_PIXEL_RGBA32_FLOAT;
        default:                              return 0;
    }
}

/* What a target in this format shows as: sRGB-encoded for the sRGB and float
 * formats, whose values are linear light. */
static int aemt_format_srgb_display(int f) {
    return f == AEMT_FORMAT_R8G8B8A8_SRGB || f == AEMT_FORMAT_R16G16B16A16_SFLOAT ||
           f == AEMT_FORMAT_R32G32B32A32_SFLOAT;
}

/* IEEE 754 half to float, subnormals, infinities and NaN included. */
static float aemt_half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
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

static float aemt_channel_value(const AemtTarget* t, const unsigned char* px, int c) {
    switch (t->format) {
        case AEMT_FORMAT_R16G16B16A16_SFLOAT: {
            uint16_t h;
            memcpy(&h, px + c * 2, sizeof(h));
            return aemt_half_to_float(h);
        }
        case AEMT_FORMAT_R32G32B32A32_SFLOAT: {
            float f;
            memcpy(&f, px + c * 4, sizeof(f));
            return f;
        }
        default:
            return (float)px[c] / 255.0f;
    }
}

static unsigned char aemt_channel_u8(const AemtTarget* t, const unsigned char* px, int c) {
    if (t->bpp == 4) return px[c];
    float v = aemt_channel_value(t, px, c);
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    return (unsigned char)(v * 255.0f + 0.5f);
}

/* Waits for every slot however long it takes (see aemt_fence_free). */
static void aemt_frames_free(AemtTarget* t) {
    for (int i = 0; i < t->frame_count; i++) {
        AemtFrame* f = &t->frames[i];
        aemt_fence_free(&f->fence);
        mt_release(f->readback);
    }
    memset(t->frames, 0, sizeof(t->frames));
    t->frame_count = 0;
    t->next_frame = 0;
    t->last_submitted = -1;
}

static int aemt_frames_alloc(AemtTarget* t, int count) {
    for (int i = 0; i < count; i++) {
        AemtFrame* f = &t->frames[i];
        t->frame_count = i + 1;
        if (aemt_fence_init(&f->fence) != AEMT_OK) goto fail;
        if (t->readback_on) {
            /* A readback buffer stays mapped: the CPU reads it every frame. */
            f->readback = aemt_make_buffer(t->dev, (size_t)t->width * (size_t)t->height * (size_t)t->bpp,
                                           &f->readback_ptr);
            if (!f->readback) goto fail;
        }
    }
    t->next_frame = 0;
    t->last_submitted = -1;
    return AEMT_OK;
fail:
    aemt_frames_free(t);
    return AEMT_ERR_OOM;
}

static void aemt_target_free_images(AemtTarget* t) {
    mt_release(t->color);
    mt_release(t->msaa);
    mt_release(t->depth);
    t->color = t->msaa = t->depth = NULL;
    t->rendered = 0;
}

/* Everything that depends on the size. The caller guarantees the target is
 * idle. */
static int aemt_target_make_images(AemtTarget* t) {
    AemtDevice* d = t->dev;
    t->color = aemt_make_texture(d, t->width, t->height, t->pixel_format, 1, 1,
                                 MTL_USAGE_RENDER_TARGET | MTL_USAGE_SHADER_READ);
    if (!t->color) return AEMT_ERR_OOM;
    if (t->samples > 1) {
        t->msaa = aemt_make_texture(d, t->width, t->height, t->pixel_format, t->samples, 1,
                                    MTL_USAGE_RENDER_TARGET);
        if (!t->msaa) return AEMT_ERR_OOM;
    }
    if (t->has_depth) {
        t->depth = aemt_make_texture(d, t->width, t->height, MTL_PIXEL_DEPTH32_FLOAT, t->samples, 1,
                                     MTL_USAGE_RENDER_TARGET);
        if (!t->depth) return AEMT_ERR_OOM;
    }
    return AEMT_OK;
}

static int aemt_check_size(int width, int height, int bpp) {
    if (width <= 0 || height <= 0) {
        return aemt_fail(AEMT_ERR_ARG, "size must be positive, got %dx%d", width, height);
    }
    if (width > AEMT_MAX_TEXTURE || height > AEMT_MAX_TEXTURE) {
        return aemt_fail(AEMT_ERR_UNSUPPORTED, "%dx%d exceeds the Metal limit of %d",
                         width, height, AEMT_MAX_TEXTURE);
    }
    if ((uint64_t)width * (uint64_t)height * (uint64_t)bpp > 0x7fffffffull) {
        return aemt_fail(AEMT_ERR_UNSUPPORTED, "%dx%d does not fit a readback", width, height);
    }
    return AEMT_OK;
}

AemtTarget* aemt_target_create(AemtDevice* d, int width, int height) {
    return aemt_target_create_format(d, width, height, AEMT_FORMAT_R8G8B8A8_UNORM, 0, 1);
}

AemtTarget* aemt_target_create_ex(AemtDevice* d, int width, int height, int want_depth, int samples) {
    return aemt_target_create_format(d, width, height, AEMT_FORMAT_R8G8B8A8_UNORM, want_depth, samples);
}

AemtTarget* aemt_target_create_format(AemtDevice* d, int width, int height, int format,
                                      int want_depth, int samples) {
    aemt_clear_error();
    if (!d) { aemt_fail(AEMT_ERR_ARG, "device is null"); return NULL; }
    int bpp = aemt_format_bpp(format);
    if (!bpp) {
        aemt_fail(AEMT_ERR_ARG,
                  "format %d is not a target format (R8G8B8A8_UNORM 37, R8G8B8A8_SRGB 43, "
                  "R16G16B16A16_SFLOAT 97, R32G32B32A32_SFLOAT 109)", format);
        return NULL;
    }
    if (aemt_check_size(width, height, bpp) != AEMT_OK) return NULL;
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8 && samples != 16) {
        aemt_fail(AEMT_ERR_ARG, "sample count must be 1, 2, 4, 8 or 16 (got %d)", samples);
        return NULL;
    }
    /* Checked, not rounded down: asking for 8x on hardware with 4x is an
     * error worth naming. */
    if (samples > 1 && !MT_SEND(BOOL, AemtUInt)(d->device, mt_sel("supportsTextureSampleCount:"), (AemtUInt)samples)) {
        aemt_fail(AEMT_ERR_UNSUPPORTED, "the device does not support %dx multisampling", samples);
        return NULL;
    }
    if (samples > 1 && format == AEMT_FORMAT_R32G32B32A32_SFLOAT && !d->msaa32) {
        aemt_fail(AEMT_ERR_UNSUPPORTED, "the device cannot resolve a multisampled 32-bit float target");
        return NULL;
    }

    AemtTarget* t = (AemtTarget*)calloc(1, sizeof(*t));
    if (!t) { aemt_fail(AEMT_ERR_OOM, "out of memory"); return NULL; }
    t->dev = d;
    t->width = width;
    t->height = height;
    t->format = format;
    t->pixel_format = aemt_pixel_format(format);
    t->bpp = bpp;
    t->samples = samples;
    t->has_depth = want_depth ? 1 : 0;
    t->readback_on = 1;
    t->index_bits = 32;
    t->timeout_ms = 5000;
    t->last_submitted = -1;
    void* pool = g_mt.pool_push();
    int rc = aemt_target_make_images(t);
    if (rc == AEMT_OK) rc = aemt_frames_alloc(t, 1);
    g_mt.pool_pop(pool);
    if (rc != AEMT_OK) {
        aemt_target_destroy(t);
        return NULL;
    }
    return t;
}

static int aemt_wait_frame(AemtTarget* t, int slot) {
    return aemt_fence_wait(&t->frames[slot].fence, t->timeout_ms);
}

static int aemt_wait_all_frames(AemtTarget* t) {
    int rc = AEMT_OK;
    for (int i = 0; i < t->frame_count; i++) {
        int one = aemt_wait_frame(t, i);
        if (one != AEMT_OK) rc = one;
    }
    return rc;
}

void aemt_target_destroy(AemtTarget* t) {
    if (!t) return;
    void* pool = g_mt.pool_push();
    aemt_frames_free(t);
    aemt_target_free_images(t);
    mt_release(t->vbuf);
    mt_release(t->ibuf);
    g_mt.pool_pop(pool);
    free(t->batch);
    free(t);
}

int aemt_target_width(const AemtTarget* t)  { return t ? t->width : 0; }
int aemt_target_height(const AemtTarget* t) { return t ? t->height : 0; }
int aemt_target_format(const AemtTarget* t) { return t ? t->format : 0; }
int aemt_target_bytes_per_pixel(const AemtTarget* t) { return t ? t->bpp : 0; }
int aemt_target_has_depth(const AemtTarget* t) { return t ? t->has_depth : 0; }
int aemt_target_samples(const AemtTarget* t) { return t ? t->samples : 0; }
int aemt_target_readback(const AemtTarget* t) { return t ? t->readback_on : 0; }
int aemt_target_frames(const AemtTarget* t) { return t ? t->frame_count : 0; }
size_t aemt_rgba_size(const AemtTarget* t) {
    return t ? (size_t)t->width * (size_t)t->height * (size_t)t->bpp : 0;
}

int aemt_target_resize(AemtTarget* t, int width, int height) {
    aemt_clear_error();
    if (!t) return aemt_fail(AEMT_ERR_ARG, "target is null");
    int rc = aemt_check_size(width, height, t->bpp);
    if (rc != AEMT_OK) return rc;
    if (width == t->width && height == t->height && t->color) return AEMT_OK;
    int frames = t->frame_count > 0 ? t->frame_count : 1;
    void* pool = g_mt.pool_push();
    aemt_frames_free(t);
    aemt_target_free_images(t);
    t->width = width;
    t->height = height;
    rc = aemt_target_make_images(t);
    if (rc == AEMT_OK) rc = aemt_frames_alloc(t, frames);
    if (rc != AEMT_OK) aemt_target_free_images(t);
    g_mt.pool_pop(pool);
    return rc;
}

int aemt_target_set_readback(AemtTarget* t, int on) {
    aemt_clear_error();
    if (!t) return aemt_fail(AEMT_ERR_ARG, "target is null");
    on = on ? 1 : 0;
    if (on == t->readback_on) return AEMT_OK;
    int frames = t->frame_count > 0 ? t->frame_count : 1;
    void* pool = g_mt.pool_push();
    aemt_frames_free(t);
    t->readback_on = on;
    int rc = aemt_frames_alloc(t, frames);
    g_mt.pool_pop(pool);
    return rc;
}

int aemt_target_set_frames(AemtTarget* t, int count) {
    aemt_clear_error();
    if (!t) return aemt_fail(AEMT_ERR_ARG, "target is null");
    if (count < 1 || count > AEMT_MAX_FRAMES) {
        return aemt_fail(AEMT_ERR_ARG, "frames in flight must be 1..%d (got %d)", AEMT_MAX_FRAMES, count);
    }
    if (count == t->frame_count) return AEMT_OK;
    void* pool = g_mt.pool_push();
    aemt_frames_free(t);
    int rc = aemt_frames_alloc(t, count);
    g_mt.pool_pop(pool);
    return rc;
}

int aemt_target_set_timeout_ms(AemtTarget* t, int ms) {
    aemt_clear_error();
    if (!t) return aemt_fail(AEMT_ERR_ARG, "target is null");
    if (ms <= 0) return aemt_fail(AEMT_ERR_ARG, "timeout must be positive (got %d)", ms);
    t->timeout_ms = ms;
    return AEMT_OK;
}

/* ------------------------------------------------------------------------ */
/* Geometry                                                                  */
/* ------------------------------------------------------------------------ */

/* Vertex and index memory is a shared buffer the GPU reads directly, so
 * geometry crosses from Aether with no staging copy; a larger reservation
 * waits for the target's frames and replaces it. */
static int aemt_grow_shared(AemtTarget* t, id* buf, unsigned char** ptr, int* capacity, int need) {
    if (need <= *capacity && *buf) return AEMT_OK;
    int rc = aemt_wait_all_frames(t);
    if (rc != AEMT_OK) return rc;
    void* pool = g_mt.pool_push();
    mt_release(*buf);
    *buf = NULL;
    *ptr = NULL;
    *capacity = 0;
    id b = aemt_make_buffer(t->dev, (size_t)need, ptr);
    g_mt.pool_pop(pool);
    if (!b) return AEMT_ERR_OOM;
    *buf = b;
    *capacity = need;
    return AEMT_OK;
}

int aemt_ae_verts_reserve_n(void* tp, int count, int floats_per_vertex) {
    AemtTarget* t = (AemtTarget*)tp;
    aemt_clear_error();
    if (!t) return aemt_fail(AEMT_ERR_ARG, "target is null");
    if (count <= 0) return aemt_fail(AEMT_ERR_ARG, "vertex count must be positive, got %d", count);
    if (floats_per_vertex <= 0) {
        return aemt_fail(AEMT_ERR_ARG, "floats per vertex must be positive, got %d", floats_per_vertex);
    }
    if ((long long)count * floats_per_vertex * 4 > 0x7fffffffLL) {
        return aemt_fail(AEMT_ERR_ARG, "vertex count %d is too large", count);
    }
    int rc = aemt_grow_shared(t, &t->vbuf, &t->vbuf_ptr, &t->vbuf_capacity, count * floats_per_vertex * 4);
    if (rc != AEMT_OK) return rc;
    t->vertex_count = count;
    t->vertex_floats = floats_per_vertex;
    return AEMT_OK;
}

int aemt_ae_verts_reserve(void* t, int count) { return aemt_ae_verts_reserve_n(t, count, 5); }

int aemt_ae_verts_set_float(void* tp, int index, double value) {
    AemtTarget* t = (AemtTarget*)tp;
    aemt_clear_error();
    if (!t || !t->vbuf_ptr) return aemt_fail(AEMT_ERR_ARG, "reserve vertices first");
    int total = t->vertex_count * t->vertex_floats;
    if (index < 0 || index >= total) {
        return aemt_fail(AEMT_ERR_ARG, "float index %d is outside 0..%d", index, total - 1);
    }
    float f = (float)value;
    memcpy(t->vbuf_ptr + (size_t)index * 4u, &f, 4);
    return AEMT_OK;
}

int aemt_ae_verts_set(void* tp, int index, double x, double y, double r, double g, double b) {
    AemtTarget* t = (AemtTarget*)tp;
    aemt_clear_error();
    if (!t || !t->vbuf_ptr) return aemt_fail(AEMT_ERR_ARG, "reserve vertices first");
    if (t->vertex_floats != 5) {
        return aemt_fail(AEMT_ERR_ARG, "verts_set writes the built-in 5-float layout; this target has %d",
                         t->vertex_floats);
    }
    if (index < 0 || index >= t->vertex_count) {
        return aemt_fail(AEMT_ERR_ARG, "vertex %d is outside 0..%d", index, t->vertex_count - 1);
    }
    float v[5] = { (float)x, (float)y, (float)r, (float)g, (float)b };
    memcpy(t->vbuf_ptr + (size_t)index * 20u, v, sizeof(v));
    return AEMT_OK;
}

int aemt_ae_indices_reserve_ex(void* tp, int count, int bits) {
    AemtTarget* t = (AemtTarget*)tp;
    aemt_clear_error();
    if (!t) return aemt_fail(AEMT_ERR_ARG, "target is null");
    if (count < 0) return aemt_fail(AEMT_ERR_ARG, "index count must not be negative");
    if (bits != 16 && bits != 32) {
        return aemt_fail(AEMT_ERR_ARG, "index width must be 16 or 32 bits (got %d)", bits);
    }
    /* Checked before it is multiplied: a count whose byte size wraps an int
     * would reserve a few bytes and then accept writes far past them. */
    if ((long long)count * (bits / 8) > 0x7fffffffLL) {
        return aemt_fail(AEMT_ERR_ARG, "index count %d is too large", count);
    }
    int need = count * (bits / 8);
    if (count > 0) {
        int rc = aemt_grow_shared(t, &t->ibuf, &t->ibuf_ptr, &t->ibuf_capacity, need);
        if (rc != AEMT_OK) return rc;
    }
    t->index_count = count;
    t->index_bits = bits;
    return AEMT_OK;
}

int aemt_ae_indices_reserve(void* t, int count) { return aemt_ae_indices_reserve_ex(t, count, 32); }

int aemt_ae_indices_set(void* tp, int index, int value) {
    AemtTarget* t = (AemtTarget*)tp;
    aemt_clear_error();
    if (!t || !t->ibuf_ptr || t->index_count == 0) return aemt_fail(AEMT_ERR_ARG, "reserve indices first");
    if (index < 0 || index >= t->index_count) {
        return aemt_fail(AEMT_ERR_ARG, "index %d is outside 0..%d", index, t->index_count - 1);
    }
    if (value < 0) return aemt_fail(AEMT_ERR_ARG, "vertex index must not be negative");
    if (value >= t->vertex_count) {
        return aemt_fail(AEMT_ERR_ARG, "index %d points past the %d uploaded vertices", value, t->vertex_count);
    }
    if (t->index_bits == 16) {
        if (value > 65535) return aemt_fail(AEMT_ERR_ARG, "vertex index %d does not fit in a 16-bit index", value);
        uint16_t v = (uint16_t)value;
        memcpy(t->ibuf_ptr + (size_t)index * 2u, &v, 2);
    } else {
        uint32_t v = (uint32_t)value;
        memcpy(t->ibuf_ptr + (size_t)index * 4u, &v, 4);
    }
    return AEMT_OK;
}

int aemt_target_set_push(AemtTarget* t, const void* data, size_t len) {
    aemt_clear_error();
    if (!t) return aemt_fail(AEMT_ERR_ARG, "target is null");
    if (len > AEMT_MAX_PUSH || (len % 4)) {
        return aemt_fail(AEMT_ERR_ARG, "push constants are at most %d bytes in whole 4-byte words, got %zu",
                         AEMT_MAX_PUSH, len);
    }
    if (len > 0 && !data) return aemt_fail(AEMT_ERR_ARG, "push data is null");
    if (len > 0) memcpy(t->push, data, len);
    t->push_size = (int)len;
    return AEMT_OK;
}

int aemt_ae_push_floats(void* tp, int count) {
    AemtTarget* t = (AemtTarget*)tp;
    aemt_clear_error();
    if (!t) return aemt_fail(AEMT_ERR_ARG, "target is null");
    if (count < 0 || (size_t)count * 4u > AEMT_MAX_PUSH) {
        return aemt_fail(AEMT_ERR_ARG, "push block of %d floats exceeds %d bytes", count, AEMT_MAX_PUSH);
    }
    memset(t->push, 0, sizeof(t->push));
    t->push_size = count * 4;
    return AEMT_OK;
}

int aemt_ae_push_float(void* tp, int index, double value) {
    AemtTarget* t = (AemtTarget*)tp;
    aemt_clear_error();
    if (!t) return aemt_fail(AEMT_ERR_ARG, "target is null");
    int n = t->push_size / 4;
    if (index < 0 || index >= n) return aemt_fail(AEMT_ERR_ARG, "push float %d is outside 0..%d", index, n - 1);
    float f = (float)value;
    memcpy(t->push + (size_t)index * 4u, &f, 4);
    return AEMT_OK;
}

int aemt_batch_reset(AemtTarget* t) {
    aemt_clear_error();
    if (!t) return aemt_fail(AEMT_ERR_ARG, "target is null");
    t->batch_count = 0;
    return AEMT_OK;
}

/* A 16-bit index draw starting at an odd index reads from an offset that is
 * not a multiple of 4 bytes, which Metal requires of an index buffer offset. */
static int aemt_check_index_start(const AemtTarget* t, int first) {
    if (t->index_count > 0 && t->index_bits == 16 && (first % 2)) {
        return aemt_fail(AEMT_ERR_ARG,
                         "a draw of 16-bit indices must start at an even index on Metal (got %d)", first);
    }
    return AEMT_OK;
}

int aemt_batch_add(AemtTarget* t, AemtMaterial* mat, int first, int count) {
    aemt_clear_error();
    if (!t) return aemt_fail(AEMT_ERR_ARG, "target is null");
    if (first < 0) return aemt_fail(AEMT_ERR_ARG, "first must not be negative, got %d", first);
    if (count <= 0) return aemt_fail(AEMT_ERR_ARG, "draw count must be positive, got %d", count);
    int limit = t->index_count > 0 ? t->index_count : t->vertex_count;
    if (limit > 0 && (long long)first + count > limit) {
        return aemt_fail(AEMT_ERR_ARG, "draw covers %d..%lld but only %d are uploaded",
                         first, (long long)first + count - 1, limit);
    }
    int rc = aemt_check_index_start(t, first);
    if (rc != AEMT_OK) return rc;
    if (t->batch_count == t->batch_cap) {
        int cap = t->batch_cap ? t->batch_cap * 2 : 8;
        AemtDrawItem* grown = (AemtDrawItem*)realloc(t->batch, (size_t)cap * sizeof(*grown));
        if (!grown) return aemt_fail(AEMT_ERR_OOM, "out of memory");
        t->batch = grown;
        t->batch_cap = cap;
    }
    t->batch[t->batch_count].mat = mat;
    t->batch[t->batch_count].first = first;
    t->batch[t->batch_count].count = count;
    t->batch_count++;
    return AEMT_OK;
}

int aemt_batch_count(const AemtTarget* t) { return t ? t->batch_count : 0; }

/* ------------------------------------------------------------------------ */
/* Vertex layouts and bindings                                               */
/* ------------------------------------------------------------------------ */

typedef struct {
    int      location;
    int      binding;
    AemtUInt format;   /* MTLVertexFormat */
    int      offset;
} AemtAttr;

struct AemtLayout {
    AemtAttr attr[AEMT_MAX_ATTRS];
    int      attr_count;
    int      stride[AEMT_MAX_BINDINGS];
    int      per_instance[AEMT_MAX_BINDINGS];
    int      declared[AEMT_MAX_BINDINGS];
    int      bind_count;
};

AemtLayout* aemt_layout_create(void) {
    aemt_clear_error();
    AemtLayout* l = (AemtLayout*)calloc(1, sizeof(*l));
    if (!l) aemt_fail(AEMT_ERR_OOM, "out of memory");
    return l;
}

void aemt_layout_destroy(AemtLayout* l) { free(l); }

int aemt_layout_binding(AemtLayout* l, int binding, int stride, int per_instance) {
    aemt_clear_error();
    if (!l) return aemt_fail(AEMT_ERR_ARG, "layout is null");
    if (binding < 0 || binding >= AEMT_MAX_BINDINGS || stride <= 0) {
        return aemt_fail(AEMT_ERR_ARG, "binding %d stride %d is not a stream (bindings 0..%d)",
                         binding, stride, AEMT_MAX_BINDINGS - 1);
    }
    if (l->declared[binding]) return aemt_fail(AEMT_ERR_ARG, "binding %d is already declared", binding);
    l->declared[binding] = 1;
    l->stride[binding] = stride;
    l->per_instance[binding] = per_instance ? 1 : 0;
    l->bind_count++;
    return AEMT_OK;
}

static AemtUInt aemt_vertex_format(int f) {
    switch (f) {
        case AEMT_FORMAT_R32_SFLOAT:          return MTL_VERTEX_FLOAT;
        case AEMT_FORMAT_R32G32_SFLOAT:       return MTL_VERTEX_FLOAT2;
        case AEMT_FORMAT_R32G32B32_SFLOAT:    return MTL_VERTEX_FLOAT3;
        case AEMT_FORMAT_R32G32B32A32_SFLOAT: return MTL_VERTEX_FLOAT4;
        case AEMT_FORMAT_R8G8B8A8_UNORM:      return MTL_VERTEX_UCHAR4_NORMALIZED;
        default:                              return 0;
    }
}

int aemt_layout_attr(AemtLayout* l, int location, int binding, int format, int offset) {
    aemt_clear_error();
    if (!l) return aemt_fail(AEMT_ERR_ARG, "layout is null");
    if (location < 0 || location >= 31 || binding < 0 || binding >= AEMT_MAX_BINDINGS || offset < 0) {
        return aemt_fail(AEMT_ERR_ARG, "location must be 0..30, binding 0..%d, and the offset not negative",
                         AEMT_MAX_BINDINGS - 1);
    }
    AemtUInt vf = aemt_vertex_format(format);
    if (!vf) {
        return aemt_fail(AEMT_ERR_ARG,
                         "format %d is not a vertex attribute format (R32_SFLOAT 100, R32G32_SFLOAT 103, "
                         "R32G32B32_SFLOAT 106, R32G32B32A32_SFLOAT 109, R8G8B8A8_UNORM 37)", format);
    }
    if (l->attr_count >= AEMT_MAX_ATTRS) return aemt_fail(AEMT_ERR_ARG, "at most %d vertex attributes", AEMT_MAX_ATTRS);
    AemtAttr* a = &l->attr[l->attr_count++];
    a->location = location;
    a->binding = binding;
    a->format = vf;
    a->offset = offset;
    return AEMT_OK;
}

enum { AEMT_BIND_NONE = 0, AEMT_BIND_UNIFORM, AEMT_BIND_TEXTURE, AEMT_BIND_STORAGE };

struct AemtBindings {
    int kind[AEMT_MAX_DESC];
    int count;
};

AemtBindings* aemt_bindings_create(void) {
    aemt_clear_error();
    AemtBindings* b = (AemtBindings*)calloc(1, sizeof(*b));
    if (!b) aemt_fail(AEMT_ERR_OOM, "out of memory");
    return b;
}

void aemt_bindings_destroy(AemtBindings* b) { free(b); }

static int aemt_bindings_add(AemtBindings* b, int binding, int kind) {
    aemt_clear_error();
    if (!b) return aemt_fail(AEMT_ERR_ARG, "bindings is null");
    if (binding < 0 || binding >= AEMT_MAX_DESC) {
        return aemt_fail(AEMT_ERR_ARG, "binding must be 0..%d", AEMT_MAX_DESC - 1);
    }
    if (b->kind[binding]) return aemt_fail(AEMT_ERR_ARG, "binding %d is already declared", binding);
    b->kind[binding] = kind;
    b->count++;
    return AEMT_OK;
}

int aemt_bindings_uniform(AemtBindings* b, int binding) { return aemt_bindings_add(b, binding, AEMT_BIND_UNIFORM); }
int aemt_bindings_texture(AemtBindings* b, int binding) { return aemt_bindings_add(b, binding, AEMT_BIND_TEXTURE); }
int aemt_bindings_storage(AemtBindings* b, int binding) { return aemt_bindings_add(b, binding, AEMT_BIND_STORAGE); }

/* ------------------------------------------------------------------------ */
/* Textures                                                                  */
/* ------------------------------------------------------------------------ */

struct AemtTexture {
    AemtDevice* dev;
    int         width, height;
    int         mips;
    id          tex;
    id          sampler;
    int         uploaded;
};

static int aemt_mip_levels_for(int w, int h) {
    int levels = 1;
    while (w > 1 || h > 1) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        levels++;
    }
    return levels;
}

AemtTexture* aemt_texture_create(AemtDevice* d, int w, int h) {
    return aemt_texture_create_ex(d, w, h, 0, 0, 0);
}

AemtTexture* aemt_texture_create_ex(AemtDevice* d, int w, int h, int mipmapped, int linear_filter, int repeat) {
    aemt_clear_error();
    if (!d) { aemt_fail(AEMT_ERR_ARG, "device is null"); return NULL; }
    if (w <= 0 || h <= 0) { aemt_fail(AEMT_ERR_ARG, "texture size %dx%d is not positive", w, h); return NULL; }
    if (w > AEMT_MAX_TEXTURE || h > AEMT_MAX_TEXTURE) {
        aemt_fail(AEMT_ERR_UNSUPPORTED, "texture %dx%d exceeds the Metal limit of %d", w, h, AEMT_MAX_TEXTURE);
        return NULL;
    }
    AemtTexture* tex = (AemtTexture*)calloc(1, sizeof(*tex));
    if (!tex) { aemt_fail(AEMT_ERR_OOM, "out of memory"); return NULL; }
    tex->dev = d;
    tex->width = w;
    tex->height = h;
    tex->mips = mipmapped ? aemt_mip_levels_for(w, h) : 1;
    void* pool = g_mt.pool_push();
    /* Render-target usage too: generating the mip chain renders into it. */
    tex->tex = aemt_make_texture(d, w, h, MTL_PIXEL_RGBA8_UNORM, 1, tex->mips,
                                 MTL_USAGE_SHADER_READ | (tex->mips > 1 ? MTL_USAGE_RENDER_TARGET : 0));
    if (tex->tex) {
        id sd = mt_new("MTLSamplerDescriptor");
        AemtUInt filter = linear_filter ? MTL_FILTER_LINEAR : MTL_FILTER_NEAREST;
        AemtUInt mip = tex->mips == 1 ? MTL_MIP_NOT_MIPMAPPED : (linear_filter ? MTL_MIP_LINEAR : MTL_MIP_NEAREST);
        AemtUInt mode = repeat ? MTL_ADDRESS_REPEAT : MTL_ADDRESS_CLAMP_TO_EDGE;
        MT_SEND(void, AemtUInt)(sd, mt_sel("setMinFilter:"), filter);
        MT_SEND(void, AemtUInt)(sd, mt_sel("setMagFilter:"), filter);
        MT_SEND(void, AemtUInt)(sd, mt_sel("setMipFilter:"), mip);
        MT_SEND(void, AemtUInt)(sd, mt_sel("setSAddressMode:"), mode);
        MT_SEND(void, AemtUInt)(sd, mt_sel("setTAddressMode:"), mode);
        MT_SEND(void, AemtUInt)(sd, mt_sel("setRAddressMode:"), mode);
        tex->sampler = MT_SEND(id, id)(d->device, mt_sel("newSamplerStateWithDescriptor:"), sd);
        mt_release(sd);
        if (!tex->sampler) aemt_fail(AEMT_ERR_OOM, "newSamplerStateWithDescriptor failed");
    }
    g_mt.pool_pop(pool);
    if (!tex->tex || !tex->sampler) {
        aemt_texture_destroy(tex);
        return NULL;
    }
    return tex;
}

int aemt_texture_mip_levels(const AemtTexture* tex) { return tex ? tex->mips : 0; }

/* A command buffer retains what it uses, so a draw still sampling the
 * texture keeps it alive past this. */
void aemt_texture_destroy(AemtTexture* tex) {
    if (!tex) return;
    void* pool = g_mt.pool_push();
    mt_release(tex->sampler);
    mt_release(tex->tex);
    g_mt.pool_pop(pool);
    free(tex);
}

/* Level 0 is copied in from a staging buffer, and a mipmapped texture's
 * chain generated on the GPU (generateMipmapsForTexture: a 2:1 filter per
 * level), all in one command buffer. */
int aemt_texture_upload(AemtTexture* tex, const void* rgba, size_t len) {
    aemt_clear_error();
    if (!tex || !rgba) return aemt_fail(AEMT_ERR_ARG, "texture or pixel data is null");
    size_t need = (size_t)tex->width * (size_t)tex->height * 4u;
    if (len < need) {
        return aemt_fail(AEMT_ERR_ARG, "need %zu bytes for %dx%d RGBA, got %zu", need, tex->width, tex->height, len);
    }
    AemtDevice* d = tex->dev;
    void* pool = g_mt.pool_push();
    unsigned char* map = NULL;
    id staging = aemt_make_buffer(d, need, &map);
    if (!staging) { g_mt.pool_pop(pool); return AEMT_ERR_OOM; }
    memcpy(map, rgba, need);
    id cb = MT_SEND(id)(d->queue, mt_sel("commandBuffer"));
    id blit = cb ? MT_SEND(id)(cb, mt_sel("blitCommandEncoder")) : NULL;
    int rc = AEMT_OK;
    if (!blit) {
        rc = aemt_fail(AEMT_ERR_OOM, "cannot make a blit encoder");
    } else {
        AemtSize size = { (AemtUInt)tex->width, (AemtUInt)tex->height, 1 };
        AemtOrigin origin = { 0, 0, 0 };
        MT_SEND(void, id, AemtUInt, AemtUInt, AemtUInt, AemtSize, id, AemtUInt, AemtUInt, AemtOrigin)(
            blit, mt_sel("copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:"
                         "toTexture:destinationSlice:destinationLevel:destinationOrigin:"),
            staging, 0, (AemtUInt)tex->width * 4u, (AemtUInt)need, size, tex->tex, 0, 0, origin);
        if (tex->mips > 1) MT_SEND(void, id)(blit, mt_sel("generateMipmapsForTexture:"), tex->tex);
        MT_SEND(void)(blit, mt_sel("endEncoding"));
        MT_SEND(void)(cb, mt_sel("commit"));
        MT_SEND(void)(cb, mt_sel("waitUntilCompleted"));
        rc = aemt_cb_status(cb);
    }
    mt_release(staging);
    g_mt.pool_pop(pool);
    if (rc == AEMT_OK) tex->uploaded = 1;
    return rc;
}

/* ------------------------------------------------------------------------ */
/* Buffers                                                                   */
/* ------------------------------------------------------------------------ */

/* A shared buffer shaders read and write: the Metal counterpart of Vulkan's
 * host-visible coherent memory, mapped for its lifetime and zeroed. Usable as
 * a storage or uniform binding. */
struct AemtBuffer {
    AemtDevice*    dev;
    id             buf;
    unsigned char* ptr;
    size_t         size;
};

AemtBuffer* aemt_buffer_create(AemtDevice* d, size_t bytes) {
    aemt_clear_error();
    if (!d) { aemt_fail(AEMT_ERR_ARG, "device is null"); return NULL; }
    if (bytes == 0) { aemt_fail(AEMT_ERR_ARG, "a buffer needs at least one byte"); return NULL; }
    AemtBuffer* b = (AemtBuffer*)calloc(1, sizeof(*b));
    if (!b) { aemt_fail(AEMT_ERR_OOM, "out of memory"); return NULL; }
    b->dev = d;
    b->size = bytes;
    void* pool = g_mt.pool_push();
    b->buf = aemt_make_buffer(d, bytes, &b->ptr);
    g_mt.pool_pop(pool);
    if (!b->buf) { free(b); return NULL; }
    return b;
}

void aemt_buffer_destroy(AemtBuffer* b) {
    if (!b) return;
    void* pool = g_mt.pool_push();
    mt_release(b->buf);
    g_mt.pool_pop(pool);
    free(b);
}

size_t aemt_buffer_size(const AemtBuffer* b) { return b ? b->size : 0; }

int aemt_buffer_write(AemtBuffer* b, size_t offset, const void* data, size_t len) {
    aemt_clear_error();
    if (!b || !data) return aemt_fail(AEMT_ERR_ARG, "buffer or data is null");
    if (offset > b->size || len > b->size - offset) {
        return aemt_fail(AEMT_ERR_ARG, "writing %zu bytes at %zu overruns a %zu-byte buffer", len, offset, b->size);
    }
    memcpy(b->ptr + offset, data, len);
    return AEMT_OK;
}

int aemt_buffer_read(AemtBuffer* b, size_t offset, void* out, size_t len) {
    aemt_clear_error();
    if (!b || !out) return aemt_fail(AEMT_ERR_ARG, "buffer or destination is null");
    if (offset > b->size || len > b->size - offset) {
        return aemt_fail(AEMT_ERR_ARG, "reading %zu bytes at %zu overruns a %zu-byte buffer", len, offset, b->size);
    }
    memcpy(out, b->ptr + offset, len);
    return AEMT_OK;
}

/* ------------------------------------------------------------------------ */
/* Pipelines and materials                                                   */
/* ------------------------------------------------------------------------ */

struct AemtMaterial {
    AemtPipeline* pipe;        /* NULL for a compute pass's own arguments */
    int           set[AEMT_MAX_DESC];
    /* Uniforms the material owns: a shared buffer each. */
    struct {
        id             buf;
        unsigned char* ptr;
        size_t         size;
    } ub[AEMT_MAX_DESC];
    AemtBuffer*   buf[AEMT_MAX_DESC];   /* a caller's buffer at this binding */
    AemtTexture*  tex[AEMT_MAX_DESC];
};

struct AemtPipeline {
    AemtDevice*   dev;
    id            pso;
    id            depth_state;   /* NULL for a target without depth */
    int           kind[AEMT_MAX_DESC];
    int           push_bytes;
    int           vertex_input;
    AemtMaterial* def;
};

AemtPipeline* aemt_pipeline_create(AemtDevice* d, AemtTarget* t, const void* vs, size_t vs_len,
                                   const void* fs, size_t fs_len) {
    return aemt_pipeline_create_ex(d, t, vs, vs_len, fs, fs_len, NULL, 0, NULL);
}

/* The vertex descriptor for a layout (or the built-in one when `layout` is
 * NULL): attribute N is [[attribute(N)]], stream B is buffer 16 + B.
 * Autoreleased. */
static id aemt_vertex_descriptor(const AemtLayout* layout) {
    id vd = MT_SEND(id)(mt_cls("MTLVertexDescriptor"), mt_sel("vertexDescriptor"));
    if (!vd) return NULL;
    id attrs = MT_SEND(id)(vd, mt_sel("attributes"));
    id layouts = MT_SEND(id)(vd, mt_sel("layouts"));
    if (!layout) {
        id a0 = mt_at(attrs, 0);
        MT_SEND(void, AemtUInt)(a0, mt_sel("setFormat:"), (AemtUInt)MTL_VERTEX_FLOAT2);
        MT_SEND(void, AemtUInt)(a0, mt_sel("setOffset:"), 0);
        MT_SEND(void, AemtUInt)(a0, mt_sel("setBufferIndex:"), (AemtUInt)AEMT_STREAM_BASE);
        id a1 = mt_at(attrs, 1);
        MT_SEND(void, AemtUInt)(a1, mt_sel("setFormat:"), (AemtUInt)MTL_VERTEX_FLOAT3);
        MT_SEND(void, AemtUInt)(a1, mt_sel("setOffset:"), 8);
        MT_SEND(void, AemtUInt)(a1, mt_sel("setBufferIndex:"), (AemtUInt)AEMT_STREAM_BASE);
        id l0 = mt_at(layouts, AEMT_STREAM_BASE);
        MT_SEND(void, AemtUInt)(l0, mt_sel("setStride:"), 20);
        MT_SEND(void, AemtUInt)(l0, mt_sel("setStepFunction:"), (AemtUInt)MTL_STEP_PER_VERTEX);
        MT_SEND(void, AemtUInt)(l0, mt_sel("setStepRate:"), 1);
        return vd;
    }
    for (int i = 0; i < layout->attr_count; i++) {
        const AemtAttr* at = &layout->attr[i];
        id a = mt_at(attrs, (AemtUInt)at->location);
        MT_SEND(void, AemtUInt)(a, mt_sel("setFormat:"), at->format);
        MT_SEND(void, AemtUInt)(a, mt_sel("setOffset:"), (AemtUInt)at->offset);
        MT_SEND(void, AemtUInt)(a, mt_sel("setBufferIndex:"), (AemtUInt)(AEMT_STREAM_BASE + at->binding));
    }
    for (int b = 0; b < AEMT_MAX_BINDINGS; b++) {
        if (!layout->declared[b]) continue;
        id l = mt_at(layouts, (AemtUInt)(AEMT_STREAM_BASE + b));
        MT_SEND(void, AemtUInt)(l, mt_sel("setStride:"), (AemtUInt)layout->stride[b]);
        MT_SEND(void, AemtUInt)(l, mt_sel("setStepFunction:"),
                                layout->per_instance[b] ? (AemtUInt)MTL_STEP_PER_INSTANCE : (AemtUInt)MTL_STEP_PER_VERTEX);
        MT_SEND(void, AemtUInt)(l, mt_sel("setStepRate:"), 1);
    }
    return vd;
}

AemtPipeline* aemt_pipeline_create_ex(AemtDevice* d, AemtTarget* t, const void* vs, size_t vs_len,
                                      const void* fs, size_t fs_len, const AemtLayout* layout,
                                      int push_bytes, const AemtBindings* bindings) {
    aemt_clear_error();
    if (!d || !t) { aemt_fail(AEMT_ERR_ARG, "device or target is null"); return NULL; }
    if (push_bytes < 0 || push_bytes > AEMT_MAX_PUSH || (push_bytes % 4)) {
        aemt_fail(AEMT_ERR_ARG, "push constant block must be 0..%d bytes and a multiple of 4 (got %d)",
                  AEMT_MAX_PUSH, push_bytes);
        return NULL;
    }
    if (layout) {
        /* A target feeds one vertex stream, binding 0 (verts_reserve fills it).
         * A layout declaring another would have the pipeline read a buffer that
         * is never bound, so it is refused here rather than drawn from. */
        for (int b = 1; b < AEMT_MAX_BINDINGS; b++) {
            if (layout->declared[b]) {
                aemt_fail(AEMT_ERR_ARG, "vertex binding %d is declared, but a target feeds binding 0 only", b);
                return NULL;
            }
        }
        for (int i = 0; i < layout->attr_count; i++) {
            if (!layout->declared[layout->attr[i].binding]) {
                aemt_fail(AEMT_ERR_ARG, "attribute %d reads binding %d, which was never declared",
                          i, layout->attr[i].binding);
                return NULL;
            }
        }
    }
    AemtPipeline* p = (AemtPipeline*)calloc(1, sizeof(*p));
    if (!p) { aemt_fail(AEMT_ERR_OOM, "out of memory"); return NULL; }
    p->dev = d;
    p->push_bytes = push_bytes;
    p->vertex_input = !layout || layout->bind_count > 0;
    for (int i = 0; bindings && i < AEMT_MAX_DESC; i++) p->kind[i] = bindings->kind[i];

    void* pool = g_mt.pool_push();
    id vlib = NULL, flib = NULL, vfn = NULL, ffn = NULL, desc = NULL;
    vlib = aemt_library(d, vs, vs_len, "vertex");
    if (vlib) flib = aemt_library(d, fs, fs_len, "fragment");
    if (flib) vfn = aemt_function(vlib, MTL_FUNCTION_VERTEX, "vertex");
    if (vfn) ffn = aemt_function(flib, MTL_FUNCTION_FRAGMENT, "fragment");
    if (ffn) {
        desc = mt_new("MTLRenderPipelineDescriptor");
        MT_SEND(void, id)(desc, mt_sel("setVertexFunction:"), vfn);
        MT_SEND(void, id)(desc, mt_sel("setFragmentFunction:"), ffn);
        if (p->vertex_input) MT_SEND(void, id)(desc, mt_sel("setVertexDescriptor:"), aemt_vertex_descriptor(layout));
        id ca = mt_at(MT_SEND(id)(desc, mt_sel("colorAttachments")), 0);
        MT_SEND(void, AemtUInt)(ca, mt_sel("setPixelFormat:"), t->pixel_format);
        if (t->has_depth) {
            MT_SEND(void, AemtUInt)(desc, mt_sel("setDepthAttachmentPixelFormat:"), (AemtUInt)MTL_PIXEL_DEPTH32_FLOAT);
        }
        MT_SEND(void, AemtUInt)(desc, mt_sel("setRasterSampleCount:"), (AemtUInt)t->samples);
        id error = NULL;
        p->pso = MT_SEND(id, id, id*)(d->device, mt_sel("newRenderPipelineStateWithDescriptor:error:"), desc, &error);
        if (!p->pso) {
            aemt_fail(AEMT_ERR_SHADER, "the render pipeline was rejected: %s "
                      "(the shaders' inputs, outputs and indices must match the layout and bindings)",
                      mt_error_text(error));
        }
    }
    if (p->pso && t->has_depth) {
        id dd = mt_new("MTLDepthStencilDescriptor");
        MT_SEND(void, AemtUInt)(dd, mt_sel("setDepthCompareFunction:"), (AemtUInt)MTL_COMPARE_LESS);
        MT_SEND(void, BOOL)(dd, mt_sel("setDepthWriteEnabled:"), (BOOL)1);
        p->depth_state = MT_SEND(id, id)(d->device, mt_sel("newDepthStencilStateWithDescriptor:"), dd);
        mt_release(dd);
        if (!p->depth_state) aemt_fail(AEMT_ERR_OOM, "newDepthStencilStateWithDescriptor failed");
    }
    mt_release(desc);
    mt_release(vfn);
    mt_release(ffn);
    mt_release(vlib);
    mt_release(flib);
    g_mt.pool_pop(pool);
    if (!p->pso || (t->has_depth && !p->depth_state)) goto fail;
    if (bindings && bindings->count > 0) {
        p->def = aemt_material_create(p);
        if (!p->def) goto fail;
    }
    return p;
fail:
    aemt_pipeline_destroy(p);
    return NULL;
}

void aemt_pipeline_destroy(AemtPipeline* p) {
    if (!p) return;
    aemt_material_destroy(p->def);
    void* pool = g_mt.pool_push();
    mt_release(p->depth_state);
    mt_release(p->pso);
    g_mt.pool_pop(pool);
    free(p);
}

AemtMaterial* aemt_material_create(AemtPipeline* p) {
    aemt_clear_error();
    if (!p) { aemt_fail(AEMT_ERR_ARG, "pipeline is null"); return NULL; }
    int any = 0;
    for (int i = 0; i < AEMT_MAX_DESC; i++) any |= p->kind[i];
    if (!any) {
        aemt_fail(AEMT_ERR_ARG, "pipeline was created without bindings, so it has no materials");
        return NULL;
    }
    AemtMaterial* m = (AemtMaterial*)calloc(1, sizeof(*m));
    if (!m) { aemt_fail(AEMT_ERR_OOM, "out of memory"); return NULL; }
    m->pipe = p;
    return m;
}

static void aemt_material_release(AemtMaterial* m) {
    void* pool = g_mt.pool_push();
    for (int i = 0; i < AEMT_MAX_DESC; i++) mt_release(m->ub[i].buf);
    g_mt.pool_pop(pool);
    memset(m->ub, 0, sizeof(m->ub));
}

void aemt_material_destroy(AemtMaterial* m) {
    if (!m) return;
    aemt_material_release(m);
    free(m);
}

static int aemt_material_check(AemtMaterial* m, int binding, int kind, const char* what) {
    if (!m) return aemt_fail(AEMT_ERR_ARG, "material is null");
    if (binding < 0 || binding >= AEMT_MAX_DESC) {
        return aemt_fail(AEMT_ERR_ARG, "binding must be 0..%d", AEMT_MAX_DESC - 1);
    }
    int k = m->pipe->kind[binding];
    if (k != kind && !(kind == AEMT_BIND_STORAGE && k == AEMT_BIND_UNIFORM)) {
        return aemt_fail(AEMT_ERR_ARG, "binding %d is not declared as %s", binding, what);
    }
    return AEMT_OK;
}

/* Writes a uniform the material owns: a shared buffer made on first use and
 * reused while big enough, so a per-frame update is a memcpy. */
int aemt_material_set_uniform(AemtMaterial* m, int binding, const void* data, size_t len) {
    aemt_clear_error();
    int rc = aemt_material_check(m, binding, AEMT_BIND_UNIFORM, "a uniform");
    if (rc != AEMT_OK) return rc;
    if (!data || len == 0) return aemt_fail(AEMT_ERR_ARG, "uniform data is empty");
    if (m->ub[binding].buf && m->ub[binding].size < len) {
        void* pool = g_mt.pool_push();
        mt_release(m->ub[binding].buf);
        g_mt.pool_pop(pool);
        memset(&m->ub[binding], 0, sizeof(m->ub[binding]));
    }
    if (!m->ub[binding].buf) {
        void* pool = g_mt.pool_push();
        m->ub[binding].buf = aemt_make_buffer(m->pipe->dev, len, &m->ub[binding].ptr);
        g_mt.pool_pop(pool);
        if (!m->ub[binding].buf) return AEMT_ERR_OOM;
        m->ub[binding].size = len;
    }
    memcpy(m->ub[binding].ptr, data, len);
    m->buf[binding] = NULL;
    m->set[binding] = 1;
    return AEMT_OK;
}

int aemt_material_set_texture(AemtMaterial* m, int binding, AemtTexture* tex) {
    aemt_clear_error();
    int rc = aemt_material_check(m, binding, AEMT_BIND_TEXTURE, "a texture");
    if (rc != AEMT_OK) return rc;
    if (!tex) return aemt_fail(AEMT_ERR_ARG, "texture is null");
    if (!tex->uploaded) return aemt_fail(AEMT_ERR_ARG, "texture has no pixels yet, upload before binding");
    m->tex[binding] = tex;
    m->set[binding] = 1;
    return AEMT_OK;
}

int aemt_material_set_buffer(AemtMaterial* m, int binding, AemtBuffer* buf) {
    aemt_clear_error();
    int rc = aemt_material_check(m, binding, AEMT_BIND_STORAGE, "a storage or uniform buffer");
    if (rc != AEMT_OK) return rc;
    if (!buf) return aemt_fail(AEMT_ERR_ARG, "buffer is null");
    if (buf->dev != m->pipe->dev) return aemt_fail(AEMT_ERR_ARG, "the buffer belongs to another device");
    m->buf[binding] = buf;
    m->set[binding] = 1;
    return AEMT_OK;
}

/* ------------------------------------------------------------------------ */
/* Drawing                                                                   */
/* ------------------------------------------------------------------------ */

/* Every binding the pipeline declares has to hold something before a draw
 * reads it. */
static int aemt_material_ready(const AemtPipeline* p, const AemtMaterial* m) {
    for (int i = 0; i < AEMT_MAX_DESC; i++) {
        if (p->kind[i] && (!m || !m->set[i])) {
            return aemt_fail(AEMT_ERR_ARG, "binding %d was declared but never set", i);
        }
    }
    return AEMT_OK;
}

/* Binds a material's resources to both stages, as Vulkan's bindings are
 * visible to every stage. */
static void aemt_bind_render(id enc, const int* kind, const AemtMaterial* m) {
    for (int i = 0; i < AEMT_MAX_DESC; i++) {
        if (!kind[i]) continue;
        if (kind[i] == AEMT_BIND_TEXTURE) {
            MT_SEND(void, id, AemtUInt)(enc, mt_sel("setVertexTexture:atIndex:"), m->tex[i]->tex, (AemtUInt)i);
            MT_SEND(void, id, AemtUInt)(enc, mt_sel("setFragmentTexture:atIndex:"), m->tex[i]->tex, (AemtUInt)i);
            MT_SEND(void, id, AemtUInt)(enc, mt_sel("setVertexSamplerState:atIndex:"), m->tex[i]->sampler, (AemtUInt)i);
            MT_SEND(void, id, AemtUInt)(enc, mt_sel("setFragmentSamplerState:atIndex:"), m->tex[i]->sampler, (AemtUInt)i);
            continue;
        }
        id b = m->buf[i] ? m->buf[i]->buf : m->ub[i].buf;
        MT_SEND(void, id, AemtUInt, AemtUInt)(enc, mt_sel("setVertexBuffer:offset:atIndex:"), b, 0, (AemtUInt)i);
        MT_SEND(void, id, AemtUInt, AemtUInt)(enc, mt_sel("setFragmentBuffer:offset:atIndex:"), b, 0, (AemtUInt)i);
    }
}

static void aemt_draw_range(AemtTarget* t, id enc, int first, int count) {
    if (t->index_count > 0) {
        AemtUInt size = (AemtUInt)(t->index_bits / 8);
        MT_SEND(void, AemtUInt, AemtUInt, AemtUInt, id, AemtUInt)(
            enc, mt_sel("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:"),
            (AemtUInt)MTL_PRIMITIVE_TRIANGLE, (AemtUInt)count,
            t->index_bits == 16 ? (AemtUInt)MTL_INDEX_UINT16 : (AemtUInt)MTL_INDEX_UINT32,
            t->ibuf, (AemtUInt)first * size);
    } else {
        MT_SEND(void, AemtUInt, AemtUInt, AemtUInt)(enc, mt_sel("drawPrimitives:vertexStart:vertexCount:"),
                                                    (AemtUInt)MTL_PRIMITIVE_TRIANGLE, (AemtUInt)first,
                                                    (AemtUInt)count);
    }
}

/* Records one frame into a new command buffer: the pass (cleared, drawn,
 * resolved when multisampled) and the readback copy. Autoreleased. */
static id aemt_record(AemtTarget* t, AemtFrame* fr, AemtPipeline* p, AemtMaterial* mat, const float clear[4]) {
    AemtDevice* d = t->dev;
    id cb = MT_SEND(id)(d->queue, mt_sel("commandBuffer"));
    if (!cb) { aemt_fail(AEMT_ERR_OOM, "cannot make a command buffer"); return NULL; }
    id rpd = MT_SEND(id)(mt_cls("MTLRenderPassDescriptor"), mt_sel("renderPassDescriptor"));
    id ca = mt_at(MT_SEND(id)(rpd, mt_sel("colorAttachments")), 0);
    AemtClearColor cc = { clear[0], clear[1], clear[2], clear[3] };
    MT_SEND(void, AemtUInt)(ca, mt_sel("setLoadAction:"), (AemtUInt)MTL_LOAD_CLEAR);
    MT_SEND(void, AemtClearColor)(ca, mt_sel("setClearColor:"), cc);
    if (t->samples > 1) {
        MT_SEND(void, id)(ca, mt_sel("setTexture:"), t->msaa);
        MT_SEND(void, id)(ca, mt_sel("setResolveTexture:"), t->color);
        MT_SEND(void, AemtUInt)(ca, mt_sel("setStoreAction:"), (AemtUInt)MTL_STORE_RESOLVE);
    } else {
        MT_SEND(void, id)(ca, mt_sel("setTexture:"), t->color);
        MT_SEND(void, AemtUInt)(ca, mt_sel("setStoreAction:"), (AemtUInt)MTL_STORE_STORE);
    }
    if (t->has_depth) {
        id da = MT_SEND(id)(rpd, mt_sel("depthAttachment"));
        MT_SEND(void, id)(da, mt_sel("setTexture:"), t->depth);
        MT_SEND(void, AemtUInt)(da, mt_sel("setLoadAction:"), (AemtUInt)MTL_LOAD_CLEAR);
        MT_SEND(void, double)(da, mt_sel("setClearDepth:"), 1.0);
        MT_SEND(void, AemtUInt)(da, mt_sel("setStoreAction:"), (AemtUInt)MTL_STORE_DONT_CARE);
    }
    id enc = MT_SEND(id, id)(cb, mt_sel("renderCommandEncoderWithDescriptor:"), rpd);
    if (!enc) { aemt_fail(AEMT_ERR_OOM, "cannot make a render encoder"); return NULL; }
    if (p && t->vertex_count > 0) {
        AemtViewport vp = { 0.0, 0.0, (double)t->width, (double)t->height, 0.0, 1.0 };
        MT_SEND(void, AemtViewport)(enc, mt_sel("setViewport:"), vp);
        MT_SEND(void, id)(enc, mt_sel("setRenderPipelineState:"), p->pso);
        if (p->depth_state) MT_SEND(void, id)(enc, mt_sel("setDepthStencilState:"), p->depth_state);
        if (p->push_bytes > 0) {
            unsigned char block[AEMT_MAX_PUSH];
            memset(block, 0, sizeof(block));
            int n = t->push_size < p->push_bytes ? t->push_size : p->push_bytes;
            if (n) memcpy(block, t->push, (size_t)n);
            MT_SEND(void, const void*, AemtUInt, AemtUInt)(enc, mt_sel("setVertexBytes:length:atIndex:"),
                                                           block, (AemtUInt)p->push_bytes, (AemtUInt)AEMT_PUSH_INDEX);
            MT_SEND(void, const void*, AemtUInt, AemtUInt)(enc, mt_sel("setFragmentBytes:length:atIndex:"),
                                                           block, (AemtUInt)p->push_bytes, (AemtUInt)AEMT_PUSH_INDEX);
        }
        if (p->vertex_input) {
            MT_SEND(void, id, AemtUInt, AemtUInt)(enc, mt_sel("setVertexBuffer:offset:atIndex:"),
                                                  t->vbuf, 0, (AemtUInt)AEMT_STREAM_BASE);
        }
        AemtMaterial* bind_mat = mat ? mat : p->def;
        if (t->batch_count > 0) {
            for (int i = 0; i < t->batch_count; i++) {
                AemtDrawItem* it = &t->batch[i];
                AemtMaterial* im = it->mat ? it->mat : bind_mat;
                if (im) aemt_bind_render(enc, p->kind, im);
                aemt_draw_range(t, enc, it->first, it->count);
            }
        } else {
            if (bind_mat) aemt_bind_render(enc, p->kind, bind_mat);
            aemt_draw_range(t, enc, 0, t->index_count > 0 ? t->index_count : t->vertex_count);
        }
    }
    MT_SEND(void)(enc, mt_sel("endEncoding"));

    if (t->readback_on) {
        id blit = MT_SEND(id)(cb, mt_sel("blitCommandEncoder"));
        if (!blit) { aemt_fail(AEMT_ERR_OOM, "cannot make a blit encoder"); return NULL; }
        AemtOrigin origin = { 0, 0, 0 };
        AemtSize size = { (AemtUInt)t->width, (AemtUInt)t->height, 1 };
        AemtUInt row = (AemtUInt)t->width * (AemtUInt)t->bpp;
        MT_SEND(void, id, AemtUInt, AemtUInt, AemtOrigin, AemtSize, id, AemtUInt, AemtUInt, AemtUInt)(
            blit, mt_sel("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:"
                         "toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:"),
            t->color, 0, 0, origin, size, fr->readback, 0, row, row * (AemtUInt)t->height);
        MT_SEND(void)(blit, mt_sel("endEncoding"));
    }
    return cb;
}

/* Records into the next slot and commits it without waiting; returns the
 * slot or a negative status. */
static int aemt_submit_frame(AemtTarget* t, AemtPipeline* p, AemtMaterial* mat, const float clear[4]) {
    if (!t->color || t->frame_count == 0) {
        return aemt_fail(AEMT_ERR_ARG, "target has no images: its last resize failed");
    }
    if (p && t->vertex_count > 0) {
        if (p->vertex_input && !t->vbuf) return aemt_fail(AEMT_ERR_ARG, "vertices were never uploaded");
        int limit = t->index_count > 0 ? t->index_count : t->vertex_count;
        for (int i = 0; i < t->batch_count; i++) {
            AemtDrawItem* it = &t->batch[i];
            if ((long long)it->first + it->count > limit) {
                return aemt_fail(AEMT_ERR_ARG, "draw %d covers %d..%lld but only %d are uploaded",
                                 i, it->first, (long long)it->first + it->count - 1, limit);
            }
            int rc = aemt_check_index_start(t, it->first);
            if (rc != AEMT_OK) return rc;
            if (it->mat && it->mat->pipe != p) {
                return aemt_fail(AEMT_ERR_ARG, "draw %d uses a material of another pipeline", i);
            }
            rc = aemt_material_ready(p, it->mat ? it->mat : (mat ? mat : p->def));
            if (rc != AEMT_OK) return rc;
        }
        if (t->batch_count == 0) {
            int rc = aemt_material_ready(p, mat ? mat : p->def);
            if (rc != AEMT_OK) return rc;
        }
    }
    int slot = t->next_frame;
    int rc = aemt_wait_frame(t, slot);
    if (rc != AEMT_OK) return rc;
    AemtFrame* fr = &t->frames[slot];
    void* pool = g_mt.pool_push();
    id cb = aemt_record(t, fr, p, mat, clear);
    if (cb) {
        pthread_mutex_lock(&t->dev->lock);
        aemt_fence_commit(&fr->fence, cb);
        pthread_mutex_unlock(&t->dev->lock);
    }
    g_mt.pool_pop(pool);
    if (!cb) return AEMT_ERR_OOM;
    t->rendered = 1;
    t->last_submitted = slot;
    t->next_frame = (slot + 1) % t->frame_count;
    return slot;
}

static int aemt_check_draw(AemtTarget* t, AemtPipeline* p, AemtMaterial* mat) {
    if (!t) return aemt_fail(AEMT_ERR_ARG, "target is null");
    if (p && p->dev != t->dev) return aemt_fail(AEMT_ERR_ARG, "pipeline belongs to another device");
    if (mat && mat->pipe != p) return aemt_fail(AEMT_ERR_ARG, "material belongs to another pipeline");
    return AEMT_OK;
}

int aemt_submit(AemtTarget* t, AemtPipeline* p, AemtMaterial* mat, float r, float g, float b, float a) {
    aemt_clear_error();
    int rc = aemt_check_draw(t, p, mat);
    if (rc != AEMT_OK) return rc;
    float clear[4] = { r, g, b, a };
    return aemt_submit_frame(t, p, mat, clear);
}

int aemt_draw(AemtTarget* t, AemtPipeline* p, AemtMaterial* mat, float r, float g, float b, float a) {
    aemt_clear_error();
    int rc = aemt_check_draw(t, p, mat);
    if (rc != AEMT_OK) return rc;
    float clear[4] = { r, g, b, a };
    int slot = aemt_submit_frame(t, p, mat, clear);
    return slot < 0 ? slot : aemt_wait_frame(t, slot);
}

int aemt_wait_all(AemtTarget* t) {
    aemt_clear_error();
    if (!t) return aemt_fail(AEMT_ERR_ARG, "target is null");
    return aemt_wait_all_frames(t);
}

/* The newest frame's readback memory, waited for. NULL with the reason set. */
static const unsigned char* aemt_readable(AemtTarget* t) {
    if (!t->readback_on) {
        aemt_fail(AEMT_ERR_ARG, "readback is off for this target (target_set_readback)");
        return NULL;
    }
    if (t->last_submitted < 0) {
        aemt_fail(AEMT_ERR_ARG, "the target has no frame yet: draw or submit first");
        return NULL;
    }
    if (aemt_wait_frame(t, t->last_submitted) != AEMT_OK) return NULL;
    return t->frames[t->last_submitted].readback_ptr;
}

static const unsigned char* aemt_px(const AemtTarget* t, const unsigned char* base, int x, int y) {
    return base + ((size_t)y * (size_t)t->width + (size_t)x) * (size_t)t->bpp;
}

int aemt_read_rgba(AemtTarget* t, void* out, size_t out_len) {
    aemt_clear_error();
    if (!t || !out) return aemt_fail(AEMT_ERR_ARG, "target or destination is null");
    size_t need = aemt_rgba_size(t);
    if (out_len < need) return aemt_fail(AEMT_ERR_ARG, "destination holds %zu bytes, the image needs %zu", out_len, need);
    const unsigned char* base = aemt_readable(t);
    if (!base) return AEMT_ERR_ARG;
    memcpy(out, base, need);
    return AEMT_OK;
}

int aemt_read_rgba8(AemtTarget* t, void* out, size_t out_len) {
    aemt_clear_error();
    if (!t || !out) return aemt_fail(AEMT_ERR_ARG, "target or destination is null");
    size_t need = (size_t)t->width * (size_t)t->height * 4u;
    if (out_len < need) return aemt_fail(AEMT_ERR_ARG, "destination holds %zu bytes, the image needs %zu", out_len, need);
    const unsigned char* base = aemt_readable(t);
    if (!base) return AEMT_ERR_ARG;
    unsigned char* dst = (unsigned char*)out;
    for (int y = 0; y < t->height; y++) {
        for (int x = 0; x < t->width; x++) {
            const unsigned char* px = aemt_px(t, base, x, y);
            for (int c = 0; c < 4; c++) *dst++ = aemt_channel_u8(t, px, c);
        }
    }
    return AEMT_OK;
}

/* ------------------------------------------------------------------------ */
/* Compute                                                                   */
/* ------------------------------------------------------------------------ */

struct AemtCompute {
    AemtDevice*   dev;
    id            pso;
    AemtUInt      max_threads;     /* the pipeline's maxTotalThreadsPerThreadgroup */
    int           kind[AEMT_MAX_DESC];
    int           push_bytes;
    unsigned char push[AEMT_MAX_PUSH];
    AemtMaterial  args;            /* the resources bound, as a material holds them */
    AemtUInt      group[3];        /* threads a group, 0 until set */
    AemtFence     fence;
    int           timeout_ms;
};

void aemt_compute_destroy(AemtCompute* c) {
    if (!c) return;
    void* pool = g_mt.pool_push();
    aemt_fence_free(&c->fence);
    for (int i = 0; i < AEMT_MAX_DESC; i++) mt_release(c->args.ub[i].buf);
    mt_release(c->pso);
    g_mt.pool_pop(pool);
    free(c);
}

AemtCompute* aemt_compute_create(AemtDevice* d, const void* cs, size_t len, const AemtBindings* bindings,
                                 int push_bytes) {
    aemt_clear_error();
    if (!d) { aemt_fail(AEMT_ERR_ARG, "device is null"); return NULL; }
    if (push_bytes < 0 || push_bytes > AEMT_MAX_PUSH || (push_bytes % 4)) {
        aemt_fail(AEMT_ERR_ARG, "push constant block must be 0..%d bytes and a multiple of 4 (got %d)",
                  AEMT_MAX_PUSH, push_bytes);
        return NULL;
    }
    AemtCompute* c = (AemtCompute*)calloc(1, sizeof(*c));
    if (!c) { aemt_fail(AEMT_ERR_OOM, "out of memory"); return NULL; }
    c->dev = d;
    c->push_bytes = push_bytes;
    c->timeout_ms = 5000;
    for (int i = 0; bindings && i < AEMT_MAX_DESC; i++) c->kind[i] = bindings->kind[i];
    if (aemt_fence_init(&c->fence) != AEMT_OK) { free(c); return NULL; }
    void* pool = g_mt.pool_push();
    id lib = aemt_library(d, cs, len, "kernel");
    id fn = lib ? aemt_function(lib, MTL_FUNCTION_KERNEL, "kernel") : NULL;
    if (fn) {
        id error = NULL;
        c->pso = MT_SEND(id, id, id*)(d->device, mt_sel("newComputePipelineStateWithFunction:error:"), fn, &error);
        if (!c->pso) aemt_fail(AEMT_ERR_SHADER, "the compute pipeline was rejected: %s", mt_error_text(error));
        else c->max_threads = MT_SEND(AemtUInt)(c->pso, mt_sel("maxTotalThreadsPerThreadgroup"));
    }
    mt_release(fn);
    mt_release(lib);
    g_mt.pool_pop(pool);
    if (!c->pso) {
        aemt_compute_destroy(c);
        return NULL;
    }
    return c;
}

int aemt_compute_set_group_size(AemtCompute* c, int x, int y, int z) {
    aemt_clear_error();
    if (!c) return aemt_fail(AEMT_ERR_ARG, "compute is null");
    if (x <= 0 || y <= 0 || z <= 0) {
        return aemt_fail(AEMT_ERR_ARG, "a group's size must be positive, got %d x %d x %d", x, y, z);
    }
    unsigned long long total = (unsigned long long)x * (unsigned long long)y * (unsigned long long)z;
    if (total > c->max_threads) {
        return aemt_fail(AEMT_ERR_UNSUPPORTED, "%d x %d x %d threads a group exceeds this pipeline's %lu",
                         x, y, z, c->max_threads);
    }
    c->group[0] = (AemtUInt)x;
    c->group[1] = (AemtUInt)y;
    c->group[2] = (AemtUInt)z;
    return AEMT_OK;
}

static int aemt_compute_binding(AemtCompute* c, int binding) {
    if (!c) return aemt_fail(AEMT_ERR_ARG, "compute is null");
    if (binding < 0 || binding >= AEMT_MAX_DESC) return aemt_fail(AEMT_ERR_ARG, "binding must be 0..%d", AEMT_MAX_DESC - 1);
    if (!c->kind[binding]) return aemt_fail(AEMT_ERR_ARG, "binding %d was not declared", binding);
    if (c->fence.submitted) return aemt_fail(AEMT_ERR_ARG, "a dispatch is in flight: compute_wait before rebinding");
    return AEMT_OK;
}

int aemt_compute_set_buffer(AemtCompute* c, int binding, AemtBuffer* buf) {
    aemt_clear_error();
    int rc = aemt_compute_binding(c, binding);
    if (rc != AEMT_OK) return rc;
    if (!buf) return aemt_fail(AEMT_ERR_ARG, "buffer is null");
    if (buf->dev != c->dev) return aemt_fail(AEMT_ERR_ARG, "the buffer belongs to another device");
    if (c->kind[binding] == AEMT_BIND_TEXTURE) {
        return aemt_fail(AEMT_ERR_ARG, "binding %d is declared as a texture, not a buffer", binding);
    }
    c->args.buf[binding] = buf;
    c->args.set[binding] = 1;
    return AEMT_OK;
}

int aemt_compute_set_texture(AemtCompute* c, int binding, AemtTexture* tex) {
    aemt_clear_error();
    int rc = aemt_compute_binding(c, binding);
    if (rc != AEMT_OK) return rc;
    if (!tex) return aemt_fail(AEMT_ERR_ARG, "texture is null");
    if (c->kind[binding] != AEMT_BIND_TEXTURE) {
        return aemt_fail(AEMT_ERR_ARG, "binding %d is declared as a buffer, not a texture", binding);
    }
    if (!tex->uploaded) return aemt_fail(AEMT_ERR_ARG, "texture has no pixels yet, upload before binding");
    c->args.tex[binding] = tex;
    c->args.set[binding] = 1;
    return AEMT_OK;
}

int aemt_compute_set_push(AemtCompute* c, const void* data, size_t len) {
    aemt_clear_error();
    if (!c) return aemt_fail(AEMT_ERR_ARG, "compute is null");
    if (len > (size_t)c->push_bytes) return aemt_fail(AEMT_ERR_ARG, "the pipeline declared %d push bytes, got %zu", c->push_bytes, len);
    if (len > 0 && !data) return aemt_fail(AEMT_ERR_ARG, "push data is null");
    if (len > 0) memcpy(c->push, data, len);
    return AEMT_OK;
}

int aemt_compute_set_timeout_ms(AemtCompute* c, int ms) {
    aemt_clear_error();
    if (!c) return aemt_fail(AEMT_ERR_ARG, "compute is null");
    if (ms <= 0) return aemt_fail(AEMT_ERR_ARG, "timeout must be positive (got %d)", ms);
    c->timeout_ms = ms;
    return AEMT_OK;
}

int aemt_dispatch_async(AemtCompute* c, int gx, int gy, int gz) {
    aemt_clear_error();
    if (!c) return aemt_fail(AEMT_ERR_ARG, "compute is null");
    if (gx <= 0 || gy <= 0 || gz <= 0) {
        return aemt_fail(AEMT_ERR_ARG, "work group counts must be positive, got %d x %d x %d", gx, gy, gz);
    }
    if (!c->group[0]) {
        return aemt_fail(AEMT_ERR_ARG, "set the threads a group with compute_set_group_size first: "
                         "MSL does not declare them in the kernel");
    }
    for (int i = 0; i < AEMT_MAX_DESC; i++) {
        if (c->kind[i] && !c->args.set[i]) return aemt_fail(AEMT_ERR_ARG, "binding %d was declared but never set", i);
    }
    int rc = aemt_fence_wait(&c->fence, c->timeout_ms);
    if (rc != AEMT_OK) return rc;
    AemtDevice* d = c->dev;
    void* pool = g_mt.pool_push();
    id cb = MT_SEND(id)(d->queue, mt_sel("commandBuffer"));
    id enc = cb ? MT_SEND(id)(cb, mt_sel("computeCommandEncoder")) : NULL;
    if (!enc) {
        g_mt.pool_pop(pool);
        return aemt_fail(AEMT_ERR_OOM, "cannot make a compute encoder");
    }
    MT_SEND(void, id)(enc, mt_sel("setComputePipelineState:"), c->pso);
    if (c->push_bytes > 0) {
        MT_SEND(void, const void*, AemtUInt, AemtUInt)(enc, mt_sel("setBytes:length:atIndex:"),
                                                       c->push, (AemtUInt)c->push_bytes, (AemtUInt)AEMT_PUSH_INDEX);
    }
    for (int i = 0; i < AEMT_MAX_DESC; i++) {
        if (!c->kind[i]) continue;
        if (c->kind[i] == AEMT_BIND_TEXTURE) {
            MT_SEND(void, id, AemtUInt)(enc, mt_sel("setTexture:atIndex:"), c->args.tex[i]->tex, (AemtUInt)i);
            MT_SEND(void, id, AemtUInt)(enc, mt_sel("setSamplerState:atIndex:"), c->args.tex[i]->sampler, (AemtUInt)i);
        } else {
            MT_SEND(void, id, AemtUInt, AemtUInt)(enc, mt_sel("setBuffer:offset:atIndex:"),
                                                  c->args.buf[i]->buf, 0, (AemtUInt)i);
        }
    }
    AemtSize groups = { (AemtUInt)gx, (AemtUInt)gy, (AemtUInt)gz };
    AemtSize threads = { c->group[0], c->group[1], c->group[2] };
    MT_SEND(void, AemtSize, AemtSize)(enc, mt_sel("dispatchThreadgroups:threadsPerThreadgroup:"), groups, threads);
    MT_SEND(void)(enc, mt_sel("endEncoding"));
    pthread_mutex_lock(&d->lock);
    aemt_fence_commit(&c->fence, cb);
    pthread_mutex_unlock(&d->lock);
    g_mt.pool_pop(pool);
    return AEMT_OK;
}

int aemt_compute_wait(AemtCompute* c) {
    aemt_clear_error();
    if (!c) return aemt_fail(AEMT_ERR_ARG, "compute is null");
    return aemt_fence_wait(&c->fence, c->timeout_ms);
}

int aemt_dispatch(AemtCompute* c, int gx, int gy, int gz) {
    int rc = aemt_dispatch_async(c, gx, gy, gz);
    if (rc != AEMT_OK) return rc;
    return aemt_compute_wait(c);
}

/* ------------------------------------------------------------------------ */
/* Presentation                                                              */
/* ------------------------------------------------------------------------ */

/* The present pass: a full-screen triangle sampling the target, which scales
 * a frame to the drawable and, drawn into an sRGB drawable, encodes it for
 * display. */
static const char k_present_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct PresentOut { float4 pos [[position]]; float2 uv; };\n"
    "vertex PresentOut present_vertex(uint id [[vertex_id]]) {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    PresentOut o;\n"
    "    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "    o.uv = uv;\n"
    "    return o;\n"
    "}\n"
    "fragment float4 present_fragment(PresentOut i [[stage_in]],\n"
    "                                 texture2d<float> src [[texture(0)]],\n"
    "                                 sampler smp [[sampler(0)]]) {\n"
    "    return src.sample(smp, i.uv);\n"
    "}\n";

/* THE DEVICE LOCK MUST BE HELD. */
static int aemt_present_pass(AemtDevice* d) {
    if (d->present_pipe[0] && d->present_pipe[1] && d->present_sampler && d->present_sampler_nearest) {
        return AEMT_OK;
    }
    id lib = aemt_library(d, k_present_msl, sizeof(k_present_msl) - 1, "present");
    if (!lib) return AEMT_ERR_SHADER;
    id vfn = aemt_function(lib, MTL_FUNCTION_VERTEX, "present vertex");
    id ffn = vfn ? aemt_function(lib, MTL_FUNCTION_FRAGMENT, "present fragment") : NULL;
    int rc = ffn ? AEMT_OK : AEMT_ERR_SHADER;
    for (int s = 0; s < 2 && rc == AEMT_OK; s++) {
        if (d->present_pipe[s]) continue;
        id desc = mt_new("MTLRenderPipelineDescriptor");
        MT_SEND(void, id)(desc, mt_sel("setVertexFunction:"), vfn);
        MT_SEND(void, id)(desc, mt_sel("setFragmentFunction:"), ffn);
        id ca = mt_at(MT_SEND(id)(desc, mt_sel("colorAttachments")), 0);
        MT_SEND(void, AemtUInt)(ca, mt_sel("setPixelFormat:"),
                                s ? (AemtUInt)MTL_PIXEL_BGRA8_UNORM_SRGB : (AemtUInt)MTL_PIXEL_BGRA8_UNORM);
        id error = NULL;
        d->present_pipe[s] = MT_SEND(id, id, id*)(d->device, mt_sel("newRenderPipelineStateWithDescriptor:error:"),
                                                  desc, &error);
        mt_release(desc);
        if (!d->present_pipe[s]) rc = aemt_fail(AEMT_ERR_SHADER, "the present pipeline failed: %s", mt_error_text(error));
    }
    for (int n = 0; n < 2 && rc == AEMT_OK; n++) {
        id* slot = n ? &d->present_sampler_nearest : &d->present_sampler;
        if (*slot) continue;
        AemtUInt filter = n ? (AemtUInt)MTL_FILTER_NEAREST : (AemtUInt)MTL_FILTER_LINEAR;
        id sd = mt_new("MTLSamplerDescriptor");
        MT_SEND(void, AemtUInt)(sd, mt_sel("setMinFilter:"), filter);
        MT_SEND(void, AemtUInt)(sd, mt_sel("setMagFilter:"), filter);
        MT_SEND(void, AemtUInt)(sd, mt_sel("setSAddressMode:"), (AemtUInt)MTL_ADDRESS_CLAMP_TO_EDGE);
        MT_SEND(void, AemtUInt)(sd, mt_sel("setTAddressMode:"), (AemtUInt)MTL_ADDRESS_CLAMP_TO_EDGE);
        *slot = MT_SEND(id, id)(d->device, mt_sel("newSamplerStateWithDescriptor:"), sd);
        mt_release(sd);
        if (!*slot) rc = aemt_fail(AEMT_ERR_OOM, "newSamplerStateWithDescriptor failed");
    }
    mt_release(vfn);
    mt_release(ffn);
    mt_release(lib);
    return rc;
}

struct AemtSwapchain {
    AemtDevice* dev;
    int         kind;
    id          view;            /* the NSView followed, or NULL for a bare layer */
    id          layer;           /* CAMetalLayer, retained */
    int         width, height;   /* reported: 0 x 0 while the view has no area */
    int         want_w, want_h;  /* a bare layer's size, as the caller gave it */
    int         srgb;            /* the drawables' encoding */
    int         vsync;
    long long   presented;
};

/* An NSView's CAMetalLayer, made and attached when it has none. Layer-HOSTING,
 * not layer-backed: the layer is set before the view is asked for one, so
 * AppKit uses it and never draws into it. Returns the layer, not retained. */
static id aemt_view_layer(id view) {
    id metal_layer = mt_cls("CAMetalLayer");
    id layer = MT_SEND(id)(view, mt_sel("layer"));
    if (!layer || !MT_SEND(BOOL, id)(layer, mt_sel("isKindOfClass:"), metal_layer)) {
        layer = MT_SEND(id)(metal_layer, mt_sel("layer"));
        MT_SEND(void, id)(view, mt_sel("setLayer:"), layer);
        MT_SEND(void, BOOL)(view, mt_sel("setWantsLayer:"), (BOOL)1);
    }
    return layer;
}

/* Follows the view: its bounds at the window's backing scale, in pixels, and
 * 0 x 0 while the window is miniaturised. A bare layer keeps the caller's
 * size. The drawables are resized to match.
 *
 * The view and its window are AppKit's, which is to say the main thread's.
 * Presenting from another thread reads the size from the layer instead,
 * whose properties Core Animation lets any thread read, and leaves the
 * layer's scale as it was last set on the main thread. */
static void aemt_sc_follow(AemtSwapchain* s) {
    int w = s->want_w, h = s->want_h;
    if (s->view && !pthread_main_np()) {
        double scale = MT_SEND(double)(s->layer, mt_sel("contentsScale"));
        if (scale <= 0.0) scale = 1.0;
        AemtCGRect b = mt_rect(s->layer, "bounds");
        w = (int)(b.width * scale + 0.5);
        h = (int)(b.height * scale + 0.5);
    } else if (s->view) {
        id win = MT_SEND(id)(s->view, mt_sel("window"));
        double scale = win ? MT_SEND(double)(win, mt_sel("backingScaleFactor")) : 1.0;
        if (scale <= 0.0) scale = 1.0;
        AemtCGRect b = mt_rect(s->view, "bounds");
        w = (int)(b.width * scale + 0.5);
        h = (int)(b.height * scale + 0.5);
        if (win && MT_SEND(BOOL)(win, mt_sel("isMiniaturized"))) w = h = 0;
        MT_SEND(void, double)(s->layer, mt_sel("setContentsScale:"), scale);
    }
    if (w <= 0 || h <= 0) {
        s->width = s->height = 0;
        return;
    }
    if (w != s->width || h != s->height) {
        AemtCGSize size = { (double)w, (double)h };
        MT_SEND(void, AemtCGSize)(s->layer, mt_sel("setDrawableSize:"), size);
    }
    s->width = w;
    s->height = h;
}

void aemt_swapchain_destroy(AemtSwapchain* s) {
    if (!s) return;
    if (s->dev) aemt_idle(s->dev);
    void* pool = g_mt.pool_push();
    mt_release(s->layer);
    g_mt.pool_pop(pool);
    free(s);
}

AemtSwapchain* aemt_swapchain_create(AemtDevice* d, int kind, void* display, void* window, int width, int height) {
    aemt_clear_error();
    (void)display;
    if (!d) { aemt_fail(AEMT_ERR_ARG, "device is null"); return NULL; }
    if (width < 0 || height < 0) { aemt_fail(AEMT_ERR_ARG, "size must not be negative, got %dx%d", width, height); return NULL; }
    if (kind != AEMT_WINDOW_NSVIEW && kind != AEMT_WINDOW_METAL_LAYER) {
        aemt_fail(kind >= 1 && kind <= 5 ? AEMT_ERR_UNSUPPORTED : AEMT_ERR_ARG,
                  "Metal presents to an NSView (kind 2) or a CAMetalLayer (kind 5), not kind %d", kind);
        return NULL;
    }
    if (!window) { aemt_fail(AEMT_ERR_ARG, "window handle is null"); return NULL; }
    if (kind == AEMT_WINDOW_NSVIEW && !pthread_main_np()) {
        aemt_fail(AEMT_ERR_ARG, "an NSView can only be given a Metal layer on the main thread");
        return NULL;
    }
    AemtSwapchain* s = (AemtSwapchain*)calloc(1, sizeof(*s));
    if (!s) { aemt_fail(AEMT_ERR_OOM, "out of memory"); return NULL; }
    s->dev = d;
    s->kind = kind;
    s->vsync = 1;
    s->want_w = width;
    s->want_h = height;
    void* pool = g_mt.pool_push();
    int rc = AEMT_OK;
    if (kind == AEMT_WINDOW_NSVIEW) {
        s->view = (id)window;
        s->layer = mt_retain(aemt_view_layer(s->view));
    } else {
        if (!MT_SEND(BOOL, id)((id)window, mt_sel("isKindOfClass:"), mt_cls("CAMetalLayer"))) {
            rc = aemt_fail(AEMT_ERR_ARG, "the window handle of kind 5 is not a CAMetalLayer");
        } else {
            s->layer = mt_retain((id)window);
        }
    }
    if (rc == AEMT_OK && !s->layer) rc = aemt_fail(AEMT_ERR_UNSUPPORTED, "QuartzCore has no CAMetalLayer");
    if (rc == AEMT_OK) {
        MT_SEND(void, id)(s->layer, mt_sel("setDevice:"), d->device);
        MT_SEND(void, AemtUInt)(s->layer, mt_sel("setPixelFormat:"), (AemtUInt)MTL_PIXEL_BGRA8_UNORM);
        MT_SEND(void, BOOL)(s->layer, mt_sel("setFramebufferOnly:"), (BOOL)1);
        MT_SEND(void, AemtUInt)(s->layer, mt_sel("setMaximumDrawableCount:"), 3);
        MT_SEND(void, BOOL)(s->layer, mt_sel("setDisplaySyncEnabled:"), (BOOL)1);
        pthread_mutex_lock(&d->lock);
        rc = aemt_present_pass(d);
        pthread_mutex_unlock(&d->lock);
    }
    if (rc == AEMT_OK) aemt_sc_follow(s);
    g_mt.pool_pop(pool);
    if (rc != AEMT_OK) {
        char reason[sizeof(g_err)];
        snprintf(reason, sizeof(reason), "%s", g_err);
        aemt_swapchain_destroy(s);
        snprintf(g_err, sizeof(g_err), "%s", reason);
        return NULL;
    }
    return s;
}

int aemt_swapchain_resize(AemtSwapchain* s, int width, int height) {
    aemt_clear_error();
    if (!s) return aemt_fail(AEMT_ERR_ARG, "swapchain is null");
    if (width < 0 || height < 0) return aemt_fail(AEMT_ERR_ARG, "size must not be negative, got %dx%d", width, height);
    /* A view's swapchain takes the view's own size; a bare layer takes the
     * numbers given, since nothing else says how large it is. */
    s->want_w = width;
    s->want_h = height;
    void* pool = g_mt.pool_push();
    aemt_sc_follow(s);
    g_mt.pool_pop(pool);
    return AEMT_OK;
}

int aemt_swapchain_set_vsync(AemtSwapchain* s, int on) {
    aemt_clear_error();
    if (!s) return aemt_fail(AEMT_ERR_ARG, "swapchain is null");
    s->vsync = on ? 1 : 0;
    void* pool = g_mt.pool_push();
    MT_SEND(void, BOOL)(s->layer, mt_sel("setDisplaySyncEnabled:"), (BOOL)s->vsync);
    g_mt.pool_pop(pool);
    return AEMT_OK;
}

int aemt_swapchain_width(const AemtSwapchain* s)  { return s ? s->width : 0; }
int aemt_swapchain_height(const AemtSwapchain* s) { return s ? s->height : 0; }
int aemt_swapchain_format(const AemtSwapchain* s) {
    if (!s) return 0;
    return s->srgb ? AEMT_FORMAT_B8G8R8A8_SRGB : AEMT_FORMAT_B8G8R8A8_UNORM;
}
long long aemt_swapchain_presented(const AemtSwapchain* s) { return s ? s->presented : 0; }

int aemt_present(AemtSwapchain* s, AemtTarget* t) {
    aemt_clear_error();
    if (!s || !t) return aemt_fail(AEMT_ERR_ARG, "swapchain or target is null");
    if (s->dev != t->dev) return aemt_fail(AEMT_ERR_ARG, "the target belongs to another device");
    if (!t->rendered) return aemt_fail(AEMT_ERR_ARG, "the target has no frame yet: draw or submit before presenting");
    AemtDevice* d = s->dev;
    void* pool = g_mt.pool_push();
    aemt_sc_follow(s);
    if (s->width == 0) { g_mt.pool_pop(pool); return AEMT_OK; }
    int srgb = aemt_format_srgb_display(t->format);
    if (srgb != s->srgb) {
        MT_SEND(void, AemtUInt)(s->layer, mt_sel("setPixelFormat:"),
                                srgb ? (AemtUInt)MTL_PIXEL_BGRA8_UNORM_SRGB : (AemtUInt)MTL_PIXEL_BGRA8_UNORM);
        s->srgb = srgb;
    }
    /* Blocks until a drawable is free (the layer keeps three), up to a
     * second, and gives nil when none came. */
    id drawable = MT_SEND(id)(s->layer, mt_sel("nextDrawable"));
    if (!drawable) {
        g_mt.pool_pop(pool);
        return aemt_fail(AEMT_ERR_UNSUPPORTED, "the layer gave no drawable");
    }
    id cb = MT_SEND(id)(d->queue, mt_sel("commandBuffer"));
    id rpd = MT_SEND(id)(mt_cls("MTLRenderPassDescriptor"), mt_sel("renderPassDescriptor"));
    id ca = mt_at(MT_SEND(id)(rpd, mt_sel("colorAttachments")), 0);
    MT_SEND(void, id)(ca, mt_sel("setTexture:"), MT_SEND(id)(drawable, mt_sel("texture")));
    MT_SEND(void, AemtUInt)(ca, mt_sel("setLoadAction:"), (AemtUInt)MTL_LOAD_DONT_CARE);
    MT_SEND(void, AemtUInt)(ca, mt_sel("setStoreAction:"), (AemtUInt)MTL_STORE_STORE);
    id enc = cb ? MT_SEND(id, id)(cb, mt_sel("renderCommandEncoderWithDescriptor:"), rpd) : NULL;
    if (!enc) {
        g_mt.pool_pop(pool);
        return aemt_fail(AEMT_ERR_OOM, "cannot make a render encoder");
    }
    AemtViewport vp = { 0.0, 0.0, (double)s->width, (double)s->height, 0.0, 1.0 };
    MT_SEND(void, AemtViewport)(enc, mt_sel("setViewport:"), vp);
    MT_SEND(void, id)(enc, mt_sel("setRenderPipelineState:"), d->present_pipe[srgb]);
    MT_SEND(void, id, AemtUInt)(enc, mt_sel("setFragmentTexture:atIndex:"), t->color, 0);
    id sampler = (t->format == AEMT_FORMAT_R32G32B32A32_SFLOAT && !d->filter32)
                     ? d->present_sampler_nearest : d->present_sampler;
    MT_SEND(void, id, AemtUInt)(enc, mt_sel("setFragmentSamplerState:atIndex:"), sampler, 0);
    MT_SEND(void, AemtUInt, AemtUInt, AemtUInt)(enc, mt_sel("drawPrimitives:vertexStart:vertexCount:"),
                                                (AemtUInt)MTL_PRIMITIVE_TRIANGLE, 0, 3);
    MT_SEND(void)(enc, mt_sel("endEncoding"));
    MT_SEND(void, id)(cb, mt_sel("presentDrawable:"), drawable);
    /* Committed on the queue that rendered the frame: Metal orders the
     * sampling after the target's writes, and the target's next frame after
     * the sampling. */
    pthread_mutex_lock(&d->lock);
    MT_SEND(void)(cb, mt_sel("commit"));
    pthread_mutex_unlock(&d->lock);
    g_mt.pool_pop(pool);
    s->presented++;
    return AEMT_OK;
}

/* ------------------------------------------------------------------------ */
/* Readers and staging that need the objects' insides                        */
/* ------------------------------------------------------------------------ */

/* Packed 0xRRGGBBAA as a non-negative 64-bit value, or -1: 64 bits so that
 * opaque white is distinct from the failure. */
int64_t aemt_ae_pixel(void* tp, int x, int y) {
    AemtTarget* t = (AemtTarget*)tp;
    aemt_clear_error();
    if (!t) { aemt_fail(AEMT_ERR_ARG, "target is null"); return -1; }
    if (x < 0 || y < 0 || x >= t->width || y >= t->height) {
        aemt_fail(AEMT_ERR_ARG, "pixel %d,%d is outside %dx%d", x, y, t->width, t->height);
        return -1;
    }
    const unsigned char* base = aemt_readable(t);
    if (!base) return -1;
    const unsigned char* px = aemt_px(t, base, x, y);
    return (int64_t)(((uint32_t)aemt_channel_u8(t, px, 0) << 24) | ((uint32_t)aemt_channel_u8(t, px, 1) << 16) |
                     ((uint32_t)aemt_channel_u8(t, px, 2) << 8) | (uint32_t)aemt_channel_u8(t, px, 3));
}

double aemt_ae_pixel_value(void* tp, int x, int y, int channel) {
    AemtTarget* t = (AemtTarget*)tp;
    aemt_clear_error();
    if (!t) { aemt_fail(AEMT_ERR_ARG, "target is null"); return (double)NAN; }
    if (x < 0 || y < 0 || x >= t->width || y >= t->height) {
        aemt_fail(AEMT_ERR_ARG, "pixel %d,%d is outside %dx%d", x, y, t->width, t->height);
        return (double)NAN;
    }
    if (channel < 0 || channel > 3) { aemt_fail(AEMT_ERR_ARG, "channel %d is not 0..3", channel); return (double)NAN; }
    const unsigned char* base = aemt_readable(t);
    if (!base) return (double)NAN;
    return (double)aemt_channel_value(t, aemt_px(t, base, x, y), channel);
}

int aemt_ae_save_ppm(void* tp, const char* path) {
    AemtTarget* t = (AemtTarget*)tp;
    aemt_clear_error();
    if (!t || !path) return aemt_fail(AEMT_ERR_ARG, "target or path is null");
    const unsigned char* base = aemt_readable(t);
    if (!base) return AEMT_ERR_ARG;
    FILE* f = fopen(path, "wb");
    if (!f) return aemt_fail(AEMT_ERR_ARG, "cannot open %s for writing", path);
    int ok = fprintf(f, "P6\n%d %d\n255\n", t->width, t->height) > 0;
    for (int y = 0; ok && y < t->height; y++) {
        for (int x = 0; ok && x < t->width; x++) {
            const unsigned char* px = aemt_px(t, base, x, y);
            unsigned char rgb[3] = { aemt_channel_u8(t, px, 0), aemt_channel_u8(t, px, 1), aemt_channel_u8(t, px, 2) };
            ok = fwrite(rgb, 1, 3, f) == 3;
        }
    }
    if (fclose(f) != 0) ok = 0;
    return ok ? AEMT_OK : aemt_fail(AEMT_ERR_ARG, "short write to %s", path);
}

/* A uniform of `count` floats, zeroed, that uniform_float then fills: a
 * caller writes a colour or a matrix as numbers, not packed bytes. */
static int aemt_floats(AemtMaterial* m, int binding, int count) {
    if (count <= 0) return aemt_fail(AEMT_ERR_ARG, "uniform float count must be positive");
    float* zero = (float*)calloc((size_t)count, sizeof(float));
    if (!zero) return aemt_fail(AEMT_ERR_OOM, "out of memory");
    int rc = aemt_material_set_uniform(m, binding, zero, (size_t)count * sizeof(float));
    free(zero);
    return rc;
}

static int aemt_float(AemtMaterial* m, int binding, int index, double value, const char* first) {
    if (!m) return aemt_fail(AEMT_ERR_ARG, "material is null");
    if (binding < 0 || binding >= AEMT_MAX_DESC) return aemt_fail(AEMT_ERR_ARG, "binding must be 0..%d", AEMT_MAX_DESC - 1);
    if (!m->ub[binding].ptr || m->buf[binding]) return aemt_fail(AEMT_ERR_ARG, "call %s for binding %d first", first, binding);
    int n = (int)(m->ub[binding].size / sizeof(float));
    if (index < 0 || index >= n) return aemt_fail(AEMT_ERR_ARG, "uniform float %d is outside 0..%d", index, n - 1);
    float f = (float)value;
    memcpy(m->ub[binding].ptr + (size_t)index * sizeof(float), &f, sizeof(f));
    return AEMT_OK;
}

static AemtMaterial* aemt_default_material(AemtPipeline* p) {
    if (!p) { aemt_fail(AEMT_ERR_ARG, "pipeline is null"); return NULL; }
    if (!p->def) aemt_fail(AEMT_ERR_ARG, "pipeline was created without bindings, so it has no materials");
    return p->def;
}

int aemt_ae_uniform_floats(void* p, int binding, int count) {
    aemt_clear_error();
    AemtMaterial* m = aemt_default_material((AemtPipeline*)p);
    return m ? aemt_floats(m, binding, count) : AEMT_ERR_ARG;
}
int aemt_ae_uniform_float(void* p, int binding, int index, double value) {
    aemt_clear_error();
    AemtMaterial* m = aemt_default_material((AemtPipeline*)p);
    return m ? aemt_float(m, binding, index, value, "uniform_floats") : AEMT_ERR_ARG;
}
int aemt_ae_material_floats(void* m, int binding, int count) {
    aemt_clear_error();
    if (!m) return aemt_fail(AEMT_ERR_ARG, "material is null");
    return aemt_floats((AemtMaterial*)m, binding, count);
}
int aemt_ae_material_float(void* m, int binding, int index, double value) {
    aemt_clear_error();
    return aemt_float((AemtMaterial*)m, binding, index, value, "material_floats");
}
int aemt_ae_set_uniform(void* p, int binding, const void* data, int len) {
    aemt_clear_error();
    if (len < 0) return aemt_fail(AEMT_ERR_ARG, "negative uniform length");
    AemtMaterial* m = aemt_default_material((AemtPipeline*)p);
    return m ? aemt_material_set_uniform(m, binding, data, (size_t)len) : AEMT_ERR_ARG;
}
int aemt_ae_set_texture(void* p, int binding, void* tex) {
    aemt_clear_error();
    AemtMaterial* m = aemt_default_material((AemtPipeline*)p);
    return m ? aemt_material_set_texture(m, binding, (AemtTexture*)tex) : AEMT_ERR_ARG;
}
int aemt_ae_set_buffer(void* p, int binding, void* buf) {
    aemt_clear_error();
    AemtMaterial* m = aemt_default_material((AemtPipeline*)p);
    return m ? aemt_material_set_buffer(m, binding, (AemtBuffer*)buf) : AEMT_ERR_ARG;
}

static int aemt_compute_push_word(AemtCompute* c, int index, const void* word) {
    aemt_clear_error();
    if (!c) return aemt_fail(AEMT_ERR_ARG, "compute is null");
    int n = c->push_bytes / 4;
    if (index < 0 || index >= n) return aemt_fail(AEMT_ERR_ARG, "push slot %d is outside 0..%d", index, n - 1);
    memcpy(c->push + (size_t)index * 4u, word, 4);
    return AEMT_OK;
}
int aemt_ae_compute_push_float(void* c, int index, double value) {
    float f = (float)value;
    return aemt_compute_push_word((AemtCompute*)c, index, &f);
}
int aemt_ae_compute_push_int(void* c, int index, int value) {
    int32_t v = (int32_t)value;
    return aemt_compute_push_word((AemtCompute*)c, index, &v);
}

#else /* not __APPLE__: Metal exists only on Apple platforms */

static int aemt_no(void) {
    return aemt_fail(AEMT_ERR_NO_LOADER, "Metal is available on macOS only");
}

struct AemtDevice { int unused; };

int         aemt_available(void) { aemt_no(); return 0; }
const char* aemt_device_name(void) { return ""; }
AemtDevice* aemt_device_create(void) { aemt_no(); return NULL; }
void        aemt_device_destroy(AemtDevice* d) { (void)d; }
int         aemt_device_unified_memory(const AemtDevice* d) { (void)d; return 0; }
AemtTarget* aemt_target_create(AemtDevice* d, int w, int h) { (void)d; (void)w; (void)h; aemt_no(); return NULL; }
AemtTarget* aemt_target_create_ex(AemtDevice* d, int w, int h, int dp, int s) {
    (void)d; (void)w; (void)h; (void)dp; (void)s; aemt_no(); return NULL;
}
AemtTarget* aemt_target_create_format(AemtDevice* d, int w, int h, int f, int dp, int s) {
    (void)d; (void)w; (void)h; (void)f; (void)dp; (void)s; aemt_no(); return NULL;
}
void   aemt_target_destroy(AemtTarget* t) { (void)t; }
int    aemt_target_width(const AemtTarget* t) { (void)t; return 0; }
int    aemt_target_height(const AemtTarget* t) { (void)t; return 0; }
int    aemt_target_format(const AemtTarget* t) { (void)t; return 0; }
int    aemt_target_bytes_per_pixel(const AemtTarget* t) { (void)t; return 0; }
int    aemt_target_has_depth(const AemtTarget* t) { (void)t; return 0; }
int    aemt_target_samples(const AemtTarget* t) { (void)t; return 0; }
int    aemt_target_resize(AemtTarget* t, int w, int h) { (void)t; (void)w; (void)h; return aemt_no(); }
int    aemt_target_set_readback(AemtTarget* t, int on) { (void)t; (void)on; return aemt_no(); }
int    aemt_target_readback(const AemtTarget* t) { (void)t; return 0; }
int    aemt_target_set_frames(AemtTarget* t, int n) { (void)t; (void)n; return aemt_no(); }
int    aemt_target_frames(const AemtTarget* t) { (void)t; return 0; }
int    aemt_target_set_timeout_ms(AemtTarget* t, int ms) { (void)t; (void)ms; return aemt_no(); }
size_t aemt_rgba_size(const AemtTarget* t) { (void)t; return 0; }
AemtLayout* aemt_layout_create(void) { aemt_no(); return NULL; }
void   aemt_layout_destroy(AemtLayout* l) { (void)l; }
int    aemt_layout_binding(AemtLayout* l, int b, int s, int i) { (void)l; (void)b; (void)s; (void)i; return aemt_no(); }
int    aemt_layout_attr(AemtLayout* l, int lo, int b, int f, int o) { (void)l; (void)lo; (void)b; (void)f; (void)o; return aemt_no(); }
AemtBindings* aemt_bindings_create(void) { aemt_no(); return NULL; }
void   aemt_bindings_destroy(AemtBindings* b) { (void)b; }
int    aemt_bindings_uniform(AemtBindings* b, int n) { (void)b; (void)n; return aemt_no(); }
int    aemt_bindings_texture(AemtBindings* b, int n) { (void)b; (void)n; return aemt_no(); }
int    aemt_bindings_storage(AemtBindings* b, int n) { (void)b; (void)n; return aemt_no(); }
AemtPipeline* aemt_pipeline_create(AemtDevice* d, AemtTarget* t, const void* v, size_t vl, const void* p, size_t pl) {
    (void)d; (void)t; (void)v; (void)vl; (void)p; (void)pl; aemt_no(); return NULL;
}
AemtPipeline* aemt_pipeline_create_ex(AemtDevice* d, AemtTarget* t, const void* v, size_t vl, const void* p,
                                      size_t pl, const AemtLayout* l, int pb, const AemtBindings* b) {
    (void)d; (void)t; (void)v; (void)vl; (void)p; (void)pl; (void)l; (void)pb; (void)b; aemt_no(); return NULL;
}
void   aemt_pipeline_destroy(AemtPipeline* p) { (void)p; }
AemtTexture* aemt_texture_create(AemtDevice* d, int w, int h) { (void)d; (void)w; (void)h; aemt_no(); return NULL; }
AemtTexture* aemt_texture_create_ex(AemtDevice* d, int w, int h, int m, int l, int r) {
    (void)d; (void)w; (void)h; (void)m; (void)l; (void)r; aemt_no(); return NULL;
}
void   aemt_texture_destroy(AemtTexture* t) { (void)t; }
int    aemt_texture_mip_levels(const AemtTexture* t) { (void)t; return 0; }
int    aemt_texture_upload(AemtTexture* t, const void* p, size_t n) { (void)t; (void)p; (void)n; return aemt_no(); }
AemtMaterial* aemt_material_create(AemtPipeline* p) { (void)p; aemt_no(); return NULL; }
void   aemt_material_destroy(AemtMaterial* m) { (void)m; }
int    aemt_material_set_uniform(AemtMaterial* m, int b, const void* d, size_t n) { (void)m; (void)b; (void)d; (void)n; return aemt_no(); }
int    aemt_material_set_texture(AemtMaterial* m, int b, AemtTexture* t) { (void)m; (void)b; (void)t; return aemt_no(); }
int    aemt_material_set_buffer(AemtMaterial* m, int b, AemtBuffer* f) { (void)m; (void)b; (void)f; return aemt_no(); }
int    aemt_target_set_push(AemtTarget* t, const void* d, size_t n) { (void)t; (void)d; (void)n; return aemt_no(); }
int    aemt_batch_reset(AemtTarget* t) { (void)t; return aemt_no(); }
int    aemt_batch_add(AemtTarget* t, AemtMaterial* m, int f, int c) { (void)t; (void)m; (void)f; (void)c; return aemt_no(); }
int    aemt_batch_count(const AemtTarget* t) { (void)t; return 0; }
int    aemt_draw(AemtTarget* t, AemtPipeline* p, AemtMaterial* m, float r, float g, float b, float a) {
    (void)t; (void)p; (void)m; (void)r; (void)g; (void)b; (void)a; return aemt_no();
}
int    aemt_submit(AemtTarget* t, AemtPipeline* p, AemtMaterial* m, float r, float g, float b, float a) {
    (void)t; (void)p; (void)m; (void)r; (void)g; (void)b; (void)a; return aemt_no();
}
int    aemt_wait_all(AemtTarget* t) { (void)t; return aemt_no(); }
int    aemt_read_rgba(AemtTarget* t, void* o, size_t n) { (void)t; (void)o; (void)n; return aemt_no(); }
int    aemt_read_rgba8(AemtTarget* t, void* o, size_t n) { (void)t; (void)o; (void)n; return aemt_no(); }
AemtBuffer* aemt_buffer_create(AemtDevice* d, size_t n) { (void)d; (void)n; aemt_no(); return NULL; }
void   aemt_buffer_destroy(AemtBuffer* b) { (void)b; }
size_t aemt_buffer_size(const AemtBuffer* b) { (void)b; return 0; }
int    aemt_buffer_write(AemtBuffer* b, size_t o, const void* d, size_t n) { (void)b; (void)o; (void)d; (void)n; return aemt_no(); }
int    aemt_buffer_read(AemtBuffer* b, size_t o, void* d, size_t n) { (void)b; (void)o; (void)d; (void)n; return aemt_no(); }
AemtCompute* aemt_compute_create(AemtDevice* d, const void* c, size_t n, const AemtBindings* b, int p) {
    (void)d; (void)c; (void)n; (void)b; (void)p; aemt_no(); return NULL;
}
void   aemt_compute_destroy(AemtCompute* c) { (void)c; }
int    aemt_compute_set_group_size(AemtCompute* c, int x, int y, int z) { (void)c; (void)x; (void)y; (void)z; return aemt_no(); }
int    aemt_compute_set_buffer(AemtCompute* c, int b, AemtBuffer* f) { (void)c; (void)b; (void)f; return aemt_no(); }
int    aemt_compute_set_texture(AemtCompute* c, int b, AemtTexture* t) { (void)c; (void)b; (void)t; return aemt_no(); }
int    aemt_compute_set_push(AemtCompute* c, const void* d, size_t n) { (void)c; (void)d; (void)n; return aemt_no(); }
int    aemt_compute_set_timeout_ms(AemtCompute* c, int ms) { (void)c; (void)ms; return aemt_no(); }
int    aemt_dispatch(AemtCompute* c, int x, int y, int z) { (void)c; (void)x; (void)y; (void)z; return aemt_no(); }
int    aemt_dispatch_async(AemtCompute* c, int x, int y, int z) { (void)c; (void)x; (void)y; (void)z; return aemt_no(); }
int    aemt_compute_wait(AemtCompute* c) { (void)c; return aemt_no(); }
AemtSwapchain* aemt_swapchain_create(AemtDevice* d, int k, void* dp, void* w, int x, int y) {
    (void)d; (void)k; (void)dp; (void)w; (void)x; (void)y; aemt_no(); return NULL;
}
void   aemt_swapchain_destroy(AemtSwapchain* s) { (void)s; }
int    aemt_swapchain_resize(AemtSwapchain* s, int w, int h) { (void)s; (void)w; (void)h; return aemt_no(); }
int    aemt_swapchain_set_vsync(AemtSwapchain* s, int on) { (void)s; (void)on; return aemt_no(); }
int    aemt_swapchain_width(const AemtSwapchain* s) { (void)s; return 0; }
int    aemt_swapchain_height(const AemtSwapchain* s) { (void)s; return 0; }
int    aemt_swapchain_format(const AemtSwapchain* s) { (void)s; return 0; }
long long aemt_swapchain_presented(const AemtSwapchain* s) { (void)s; return 0; }
int    aemt_present(AemtSwapchain* s, AemtTarget* t) { (void)s; (void)t; return aemt_no(); }

int    aemt_ae_verts_reserve_n(void* t, int c, int f) { (void)t; (void)c; (void)f; return aemt_no(); }
int    aemt_ae_verts_reserve(void* t, int c) { (void)t; (void)c; return aemt_no(); }
int    aemt_ae_verts_set_float(void* t, int i, double v) { (void)t; (void)i; (void)v; return aemt_no(); }
int    aemt_ae_verts_set(void* t, int i, double x, double y, double r, double g, double b) {
    (void)t; (void)i; (void)x; (void)y; (void)r; (void)g; (void)b; return aemt_no();
}
int    aemt_ae_indices_reserve_ex(void* t, int c, int b) { (void)t; (void)c; (void)b; return aemt_no(); }
int    aemt_ae_indices_reserve(void* t, int c) { (void)t; (void)c; return aemt_no(); }
int    aemt_ae_indices_set(void* t, int i, int v) { (void)t; (void)i; (void)v; return aemt_no(); }
int    aemt_ae_push_floats(void* t, int c) { (void)t; (void)c; return aemt_no(); }
int    aemt_ae_push_float(void* t, int i, double v) { (void)t; (void)i; (void)v; return aemt_no(); }
int64_t aemt_ae_pixel(void* t, int x, int y) { (void)t; (void)x; (void)y; aemt_no(); return -1; }
double aemt_ae_pixel_value(void* t, int x, int y, int c) { (void)t; (void)x; (void)y; (void)c; aemt_no(); return (double)NAN; }
int    aemt_ae_save_ppm(void* t, const char* p) { (void)t; (void)p; return aemt_no(); }
int    aemt_ae_uniform_floats(void* p, int b, int c) { (void)p; (void)b; (void)c; return aemt_no(); }
int    aemt_ae_uniform_float(void* p, int b, int i, double v) { (void)p; (void)b; (void)i; (void)v; return aemt_no(); }
int    aemt_ae_material_floats(void* m, int b, int c) { (void)m; (void)b; (void)c; return aemt_no(); }
int    aemt_ae_material_float(void* m, int b, int i, double v) { (void)m; (void)b; (void)i; (void)v; return aemt_no(); }
int    aemt_ae_set_uniform(void* p, int b, const void* d, int n) { (void)p; (void)b; (void)d; (void)n; return aemt_no(); }
int    aemt_ae_set_texture(void* p, int b, void* t) { (void)p; (void)b; (void)t; return aemt_no(); }
int    aemt_ae_set_buffer(void* p, int b, void* f) { (void)p; (void)b; (void)f; return aemt_no(); }
int    aemt_ae_compute_push_float(void* c, int i, double v) { (void)c; (void)i; (void)v; return aemt_no(); }
int    aemt_ae_compute_push_int(void* c, int i, int v) { (void)c; (void)i; (void)v; return aemt_no(); }

#endif /* __APPLE__ */

/* ------------------------------------------------------------------------ */
/* Aether-facing entry points                                                */
/* ------------------------------------------------------------------------ */
/* Signatures match what aetherc emits for the externs in module.ae: ptr is
 * void*, string is const char*, int is int and float is double. */

int         aemt_ae_available(void)          { return aemt_available(); }
const char* aemt_ae_last_error(void)         { return aemt_last_error(); }
const char* aemt_ae_device_name(void)        { return aemt_device_name(); }
void*       aemt_ae_device_create(void)      { return (void*)aemt_device_create(); }
void        aemt_ae_device_destroy(void* d)  { aemt_device_destroy((AemtDevice*)d); }
int         aemt_ae_device_unified_memory(void* d) { return aemt_device_unified_memory((const AemtDevice*)d); }

void* aemt_ae_target_create(void* d, int w, int h) { return (void*)aemt_target_create((AemtDevice*)d, w, h); }
void* aemt_ae_target_create_ex(void* d, int w, int h, int depth, int samples) {
    return (void*)aemt_target_create_ex((AemtDevice*)d, w, h, depth, samples);
}
void* aemt_ae_target_create_format(void* d, int w, int h, int format, int depth, int samples) {
    return (void*)aemt_target_create_format((AemtDevice*)d, w, h, format, depth, samples);
}
void  aemt_ae_target_destroy(void* t)        { aemt_target_destroy((AemtTarget*)t); }
int   aemt_ae_target_width(void* t)          { return aemt_target_width((const AemtTarget*)t); }
int   aemt_ae_target_height(void* t)         { return aemt_target_height((const AemtTarget*)t); }
int   aemt_ae_target_format(void* t)         { return aemt_target_format((const AemtTarget*)t); }
int   aemt_ae_target_bytes_per_pixel(void* t) { return aemt_target_bytes_per_pixel((const AemtTarget*)t); }
int   aemt_ae_target_has_depth(void* t)      { return aemt_target_has_depth((const AemtTarget*)t); }
int   aemt_ae_target_samples(void* t)        { return aemt_target_samples((const AemtTarget*)t); }
int   aemt_ae_target_resize(void* t, int w, int h) { return aemt_target_resize((AemtTarget*)t, w, h); }
int   aemt_ae_target_set_readback(void* t, int on) { return aemt_target_set_readback((AemtTarget*)t, on); }
int   aemt_ae_target_readback(void* t)       { return aemt_target_readback((const AemtTarget*)t); }
int   aemt_ae_target_set_frames(void* t, int n) { return aemt_target_set_frames((AemtTarget*)t, n); }
int   aemt_ae_target_frames(void* t)         { return aemt_target_frames((const AemtTarget*)t); }
int   aemt_ae_target_set_timeout_ms(void* t, int ms) { return aemt_target_set_timeout_ms((AemtTarget*)t, ms); }
int   aemt_ae_rgba_size(void* t) {
    size_t n = aemt_rgba_size((const AemtTarget*)t);
    return n > 0x7fffffffu ? 0x7fffffff : (int)n;
}

void* aemt_ae_pipeline_create(void* d, void* t, const char* vs, int vl, const char* fs, int fl) {
    if (vl < 0 || fl < 0) { aemt_fail(AEMT_ERR_SHADER, "negative shader length"); return NULL; }
    return (void*)aemt_pipeline_create((AemtDevice*)d, (AemtTarget*)t, vs, (size_t)vl, fs, (size_t)fl);
}
void* aemt_ae_pipeline_create_ex(void* d, void* t, const char* vs, int vl, const char* fs, int fl,
                                 void* layout, int push_bytes, void* bindings) {
    if (vl < 0 || fl < 0) { aemt_fail(AEMT_ERR_SHADER, "negative shader length"); return NULL; }
    return (void*)aemt_pipeline_create_ex((AemtDevice*)d, (AemtTarget*)t, vs, (size_t)vl, fs, (size_t)fl,
                                          (const AemtLayout*)layout, push_bytes, (const AemtBindings*)bindings);
}
void  aemt_ae_pipeline_destroy(void* p)      { aemt_pipeline_destroy((AemtPipeline*)p); }

void* aemt_ae_layout_create(void)            { return (void*)aemt_layout_create(); }
void  aemt_ae_layout_destroy(void* l)        { aemt_layout_destroy((AemtLayout*)l); }
int   aemt_ae_layout_binding(void* l, int b, int stride, int per_instance) {
    return aemt_layout_binding((AemtLayout*)l, b, stride, per_instance);
}
int   aemt_ae_layout_attr(void* l, int location, int b, int format, int offset) {
    return aemt_layout_attr((AemtLayout*)l, location, b, format, offset);
}
void* aemt_ae_bindings_create(void)          { return (void*)aemt_bindings_create(); }
void  aemt_ae_bindings_destroy(void* b)      { aemt_bindings_destroy((AemtBindings*)b); }
int   aemt_ae_bindings_uniform(void* b, int n) { return aemt_bindings_uniform((AemtBindings*)b, n); }
int   aemt_ae_bindings_texture(void* b, int n) { return aemt_bindings_texture((AemtBindings*)b, n); }
int   aemt_ae_bindings_storage(void* b, int n) { return aemt_bindings_storage((AemtBindings*)b, n); }

void* aemt_ae_texture_create(void* d, int w, int h) { return (void*)aemt_texture_create((AemtDevice*)d, w, h); }
void* aemt_ae_texture_create_ex(void* d, int w, int h, int mipmapped, int linear, int repeat) {
    return (void*)aemt_texture_create_ex((AemtDevice*)d, w, h, mipmapped, linear, repeat);
}
void  aemt_ae_texture_destroy(void* t)       { aemt_texture_destroy((AemtTexture*)t); }
int   aemt_ae_texture_mip_levels(void* t)    { return aemt_texture_mip_levels((const AemtTexture*)t); }
int   aemt_ae_texture_upload(void* t, const void* rgba, int len) {
    if (len < 0) return aemt_fail(AEMT_ERR_ARG, "negative pixel length");
    return aemt_texture_upload((AemtTexture*)t, rgba, (size_t)len);
}

void* aemt_ae_material_create(void* p)       { return (void*)aemt_material_create((AemtPipeline*)p); }
void  aemt_ae_material_destroy(void* m)      { aemt_material_destroy((AemtMaterial*)m); }
int   aemt_ae_material_set_texture(void* m, int b, void* t) {
    return aemt_material_set_texture((AemtMaterial*)m, b, (AemtTexture*)t);
}
int   aemt_ae_material_set_uniform(void* m, int b, const void* data, int len) {
    if (len < 0) return aemt_fail(AEMT_ERR_ARG, "negative uniform length");
    return aemt_material_set_uniform((AemtMaterial*)m, b, data, (size_t)len);
}
int   aemt_ae_material_set_buffer(void* m, int b, void* buf) {
    return aemt_material_set_buffer((AemtMaterial*)m, b, (AemtBuffer*)buf);
}

int   aemt_ae_set_push(void* t, const void* data, int len) {
    if (len < 0) return aemt_fail(AEMT_ERR_ARG, "negative push length");
    return aemt_target_set_push((AemtTarget*)t, data, (size_t)len);
}
int   aemt_ae_batch_reset(void* t)           { return aemt_batch_reset((AemtTarget*)t); }
int   aemt_ae_batch_add(void* t, void* m, int first, int count) {
    return aemt_batch_add((AemtTarget*)t, (AemtMaterial*)m, first, count);
}
int   aemt_ae_batch_count(void* t)           { return aemt_batch_count((const AemtTarget*)t); }
int   aemt_ae_draw(void* t, void* p, double r, double g, double b, double a) {
    return aemt_draw((AemtTarget*)t, (AemtPipeline*)p, NULL, (float)r, (float)g, (float)b, (float)a);
}
int   aemt_ae_draw_material(void* t, void* p, void* m, double r, double g, double b, double a) {
    return aemt_draw((AemtTarget*)t, (AemtPipeline*)p, (AemtMaterial*)m, (float)r, (float)g, (float)b, (float)a);
}
int   aemt_ae_submit(void* t, void* p, double r, double g, double b, double a) {
    return aemt_submit((AemtTarget*)t, (AemtPipeline*)p, NULL, (float)r, (float)g, (float)b, (float)a);
}
int   aemt_ae_submit_material(void* t, void* p, void* m, double r, double g, double b, double a) {
    return aemt_submit((AemtTarget*)t, (AemtPipeline*)p, (AemtMaterial*)m, (float)r, (float)g, (float)b, (float)a);
}
int   aemt_ae_wait_all(void* t)              { return aemt_wait_all((AemtTarget*)t); }
int   aemt_ae_copy_rgba(void* t, void* dest, int len) {
    if (len < 0) return aemt_fail(AEMT_ERR_ARG, "negative destination length");
    return aemt_read_rgba((AemtTarget*)t, dest, (size_t)len);
}
int   aemt_ae_copy_rgba8(void* t, void* dest, int len) {
    if (len < 0) return aemt_fail(AEMT_ERR_ARG, "negative destination length");
    return aemt_read_rgba8((AemtTarget*)t, dest, (size_t)len);
}

void* aemt_ae_buffer_create(void* d, int bytes) {
    if (bytes < 0) { aemt_fail(AEMT_ERR_ARG, "negative buffer size"); return NULL; }
    return (void*)aemt_buffer_create((AemtDevice*)d, (size_t)bytes);
}
void  aemt_ae_buffer_destroy(void* b)        { aemt_buffer_destroy((AemtBuffer*)b); }
int   aemt_ae_buffer_size(void* b) {
    size_t n = aemt_buffer_size((const AemtBuffer*)b);
    return n > 0x7fffffffu ? 0x7fffffff : (int)n;
}
int   aemt_ae_buffer_write(void* b, int offset, const void* data, int len) {
    if (offset < 0 || len < 0) return aemt_fail(AEMT_ERR_ARG, "negative offset or length");
    return aemt_buffer_write((AemtBuffer*)b, (size_t)offset, data, (size_t)len);
}
int   aemt_ae_buffer_read(void* b, int offset, void* dest, int len) {
    if (offset < 0 || len < 0) return aemt_fail(AEMT_ERR_ARG, "negative offset or length");
    return aemt_buffer_read((AemtBuffer*)b, (size_t)offset, dest, (size_t)len);
}
int   aemt_ae_buffer_set_float(void* b, int index, double value) {
    float f = (float)value;
    if (index < 0) return aemt_fail(AEMT_ERR_ARG, "negative index");
    return aemt_buffer_write((AemtBuffer*)b, (size_t)index * 4u, &f, sizeof(f));
}
double aemt_ae_buffer_float(void* b, int index) {
    float f = 0.0f;
    if (index < 0) { aemt_fail(AEMT_ERR_ARG, "negative index"); return (double)NAN; }
    if (aemt_buffer_read((AemtBuffer*)b, (size_t)index * 4u, &f, sizeof(f)) != AEMT_OK) return (double)NAN;
    return (double)f;
}
int   aemt_ae_buffer_set_int(void* b, int index, int value) {
    int32_t v = (int32_t)value;
    if (index < 0) return aemt_fail(AEMT_ERR_ARG, "negative index");
    return aemt_buffer_write((AemtBuffer*)b, (size_t)index * 4u, &v, sizeof(v));
}
int   aemt_ae_buffer_int(void* b, int index) {
    int32_t v = 0;
    if (index < 0) { aemt_fail(AEMT_ERR_ARG, "negative index"); return 0; }
    if (aemt_buffer_read((AemtBuffer*)b, (size_t)index * 4u, &v, sizeof(v)) != AEMT_OK) return 0;
    return (int)v;
}

void* aemt_ae_compute_create(void* d, const char* cs, int len, void* bindings, int push_bytes) {
    if (len < 0) { aemt_fail(AEMT_ERR_SHADER, "negative shader length"); return NULL; }
    return (void*)aemt_compute_create((AemtDevice*)d, cs, (size_t)len, (const AemtBindings*)bindings, push_bytes);
}
void  aemt_ae_compute_destroy(void* c)       { aemt_compute_destroy((AemtCompute*)c); }
int   aemt_ae_compute_set_group_size(void* c, int x, int y, int z) {
    return aemt_compute_set_group_size((AemtCompute*)c, x, y, z);
}
int   aemt_ae_compute_set_buffer(void* c, int b, void* buf) {
    return aemt_compute_set_buffer((AemtCompute*)c, b, (AemtBuffer*)buf);
}
int   aemt_ae_compute_set_texture(void* c, int b, void* t) {
    return aemt_compute_set_texture((AemtCompute*)c, b, (AemtTexture*)t);
}
int   aemt_ae_compute_set_push(void* c, const void* data, int len) {
    if (len < 0) return aemt_fail(AEMT_ERR_ARG, "negative push length");
    return aemt_compute_set_push((AemtCompute*)c, data, (size_t)len);
}
int   aemt_ae_compute_set_timeout_ms(void* c, int ms) { return aemt_compute_set_timeout_ms((AemtCompute*)c, ms); }
int   aemt_ae_dispatch(void* c, int x, int y, int z) { return aemt_dispatch((AemtCompute*)c, x, y, z); }
int   aemt_ae_dispatch_async(void* c, int x, int y, int z) { return aemt_dispatch_async((AemtCompute*)c, x, y, z); }
int   aemt_ae_compute_wait(void* c)          { return aemt_compute_wait((AemtCompute*)c); }

void* aemt_ae_swapchain_create(void* d, int kind, void* display, void* window, int w, int h) {
    return (void*)aemt_swapchain_create((AemtDevice*)d, kind, display, window, w, h);
}
void  aemt_ae_swapchain_destroy(void* s)     { aemt_swapchain_destroy((AemtSwapchain*)s); }
int   aemt_ae_swapchain_resize(void* s, int w, int h) { return aemt_swapchain_resize((AemtSwapchain*)s, w, h); }
int   aemt_ae_swapchain_set_vsync(void* s, int on) { return aemt_swapchain_set_vsync((AemtSwapchain*)s, on); }
int   aemt_ae_swapchain_width(void* s)       { return aemt_swapchain_width((const AemtSwapchain*)s); }
int   aemt_ae_swapchain_height(void* s)      { return aemt_swapchain_height((const AemtSwapchain*)s); }
int   aemt_ae_swapchain_format(void* s)      { return aemt_swapchain_format((const AemtSwapchain*)s); }
int   aemt_ae_swapchain_presented(void* s) {
    long long n = aemt_swapchain_presented((const AemtSwapchain*)s);
    return n > 0x7fffffffLL ? 0x7fffffff : (int)n;
}
int   aemt_ae_present(void* s, void* t)      { return aemt_present((AemtSwapchain*)s, (AemtTarget*)t); }
