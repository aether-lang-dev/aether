/* contrib/d3d12: GPU rendering, compute and presentation with Direct3D 12.
 * See aether_d3d12.h for the API, the register mapping and the conventions.
 */

#include "aether_d3d12.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#  define AEDX_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define AEDX_THREAD_LOCAL __thread
#else
#  define AEDX_THREAD_LOCAL
#endif

static AEDX_THREAD_LOCAL char g_err[512];

static int aedx_fail(int code, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
    return code;
}

static void aedx_clear_error(void) { g_err[0] = '\0'; }

const char* aedx_last_error(void) { return g_err; }

/* A pipeline's fixed-function state (#2385), the same in every module: how
 * it blends, what it culls and its depth test. Zeroed is the state every
 * pipeline had before: no blending, no culling, and with depth a LESS test
 * that writes. Plain data, so it is built the same on every platform. */
struct AedxState {
    int blend;          /* 0 none, 1 alpha, 2 premultiplied, 3 additive */
    int cull;           /* 0 none, 1 back, 2 front */
    int depth_set;      /* state_depth was called */
    int depth_op;       /* compare op 1..4 */
    int depth_write;
    int topology;       /* 0 triangles, 1 strip, 2 lines, 3 line strip, 4 points (#2398) */
    /* The stencil test (#2399): compare op 1..8, reference, the three
     * STENCIL_* ops (Vulkan's numbering, one below D3D12_STENCIL_OP's) and
     * the two masks, for front and back faces alike. */
    int stencil_set;
    int stencil_op;
    int stencil_ref;
    int stencil_pass, stencil_fail, stencil_depth_fail;
    int stencil_read, stencil_write;
    /* The channels a draw writes (#2411): COLOR_* bits, D3D12_COLOR_WRITE_
     * ENABLE's own; all four unless set. */
    int color_mask_set;
    int color_mask;
};

AedxState* aedx_state_create(void) {
    aedx_clear_error();
    AedxState* s = (AedxState*)calloc(1, sizeof(*s));
    if (!s) aedx_fail(AEDX_ERR_OOM, "out of memory");
    return s;
}

void aedx_state_destroy(AedxState* s) { free(s); }

int aedx_state_blend(AedxState* s, int mode) {
    aedx_clear_error();
    if (!s) return aedx_fail(AEDX_ERR_ARG, "state is null");
    if (mode < 0 || mode > 3) {
        return aedx_fail(AEDX_ERR_ARG, "blend mode %d is not BLEND_NONE..BLEND_ADDITIVE (0..3)", mode);
    }
    s->blend = mode;
    return AEDX_OK;
}

/* A face is front-facing when its corners run counter-clockwise on screen,
 * in all three modules. */
int aedx_state_cull(AedxState* s, int mode) {
    aedx_clear_error();
    if (!s) return aedx_fail(AEDX_ERR_ARG, "state is null");
    if (mode < 0 || mode > 2) {
        return aedx_fail(AEDX_ERR_ARG, "cull mode %d is not CULL_NONE..CULL_FRONT (0..2)", mode);
    }
    s->cull = mode;
    return AEDX_OK;
}

/* The stencil test (#2399), as contrib.vulkan's state_stencil. */
int aedx_state_stencil(AedxState* s, int compare, int ref, int pass_op, int fail_op, int depth_fail_op,
                       int read_mask, int write_mask) {
    aedx_clear_error();
    if (!s) return aedx_fail(AEDX_ERR_ARG, "state is null");
    if (compare < 1 || compare > 8) {
        return aedx_fail(AEDX_ERR_ARG, "compare op %d is not COMPARE_LESS..COMPARE_NEVER (1..8)", compare);
    }
    if (pass_op < 0 || pass_op > 7 || fail_op < 0 || fail_op > 7 || depth_fail_op < 0 || depth_fail_op > 7) {
        return aedx_fail(AEDX_ERR_ARG, "stencil ops are STENCIL_KEEP..STENCIL_DECREMENT_WRAP (0..7), not %d, %d, %d",
                         pass_op, fail_op, depth_fail_op);
    }
    if (ref < 0 || ref > 255 || read_mask < 0 || read_mask > 255 || write_mask < 0 || write_mask > 255) {
        return aedx_fail(AEDX_ERR_ARG, "the reference and masks are 0..255, not %d, %d, %d",
                         ref, read_mask, write_mask);
    }
    s->stencil_set = 1;
    s->stencil_op = compare;
    s->stencil_ref = ref;
    s->stencil_pass = pass_op;
    s->stencil_fail = fail_op;
    s->stencil_depth_fail = depth_fail_op;
    s->stencil_read = read_mask;
    s->stencil_write = write_mask;
    return AEDX_OK;
}

/* The channels a draw writes (#2411), as contrib.vulkan's. */
int aedx_state_color_mask(AedxState* s, int mask) {
    aedx_clear_error();
    if (!s) return aedx_fail(AEDX_ERR_ARG, "state is null");
    if (mask < 0 || mask > 15) {
        return aedx_fail(AEDX_ERR_ARG, "a colour mask is COLOR_* bits, 0..15, not %d", mask);
    }
    s->color_mask_set = 1;
    s->color_mask = mask;
    return AEDX_OK;
}

/* What the vertices assemble into (#2398). */
int aedx_state_topology(AedxState* s, int topology) {
    aedx_clear_error();
    if (!s) return aedx_fail(AEDX_ERR_ARG, "state is null");
    if (topology < 0 || topology > 4) {
        return aedx_fail(AEDX_ERR_ARG, "topology %d is not TOPOLOGY_TRIANGLES..TOPOLOGY_POINTS (0..4)", topology);
    }
    s->topology = topology;
    return AEDX_OK;
}

int aedx_state_depth(AedxState* s, int op, int write) {
    aedx_clear_error();
    if (!s) return aedx_fail(AEDX_ERR_ARG, "state is null");
    if (op < 1 || op > 8) {
        return aedx_fail(AEDX_ERR_ARG, "compare op %d is not COMPARE_LESS..COMPARE_NEVER (1..8)", op);
    }
    s->depth_set = 1;
    s->depth_op = op;
    s->depth_write = write ? 1 : 0;
    return AEDX_OK;
}

#if defined(_WIN32)

/* The C interface to the COM headers. WIDL_C_INLINE_WRAPPERS makes MinGW's
 * headers define the methods that return a struct (a descriptor handle, a
 * resource description) as inline functions that pass the hidden return
 * pointer the way the C++ ABI does; without it those macros refuse to
 * compile, because the naive C call would read garbage. initguid.h makes the
 * interface IDs data in this file, so nothing links dxguid. */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

/* ------------------------------------------------------------------------ */
/* Runtime loading                                                           */
/* ------------------------------------------------------------------------ */

typedef HRESULT (WINAPI *AedxCreateFactory2)(UINT, REFIID, void**);
typedef HRESULT (WINAPI *AedxCreateFactory1)(REFIID, void**);

static struct {
    int                                  loaded;   /* 0 unprobed, 1 ok, -1 not */
    HMODULE                              d3d12, dxgi, compiler;
    PFN_D3D12_CREATE_DEVICE              create_device;
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE   serialize_root;
    PFN_D3D12_GET_DEBUG_INTERFACE        get_debug;
    AedxCreateFactory2                   create_factory2;
    AedxCreateFactory1                   create_factory1;
    pD3DCompile                          compile;
} g_dx;

static SRWLOCK g_load_lock = SRWLOCK_INIT;

static int aedx_load(void) {
    AcquireSRWLockExclusive(&g_load_lock);
    if (g_dx.loaded) {
        int ok = g_dx.loaded > 0;
        ReleaseSRWLockExclusive(&g_load_lock);
        if (!ok) return aedx_fail(AEDX_ERR_NO_LOADER, "Direct3D 12 is not available on this system");
        return AEDX_OK;
    }
    g_dx.loaded = -1;
    g_dx.d3d12 = LoadLibraryA("d3d12.dll");
    g_dx.dxgi = LoadLibraryA("dxgi.dll");
    g_dx.compiler = LoadLibraryA("d3dcompiler_47.dll");
    int rc = AEDX_OK;
    if (!g_dx.d3d12 || !g_dx.dxgi) {
        rc = aedx_fail(AEDX_ERR_NO_LOADER, "d3d12.dll or dxgi.dll is missing (Windows 10 or later is needed)");
    } else {
        g_dx.create_device = (PFN_D3D12_CREATE_DEVICE)(void*)GetProcAddress(g_dx.d3d12, "D3D12CreateDevice");
        g_dx.serialize_root = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void*)
            GetProcAddress(g_dx.d3d12, "D3D12SerializeRootSignature");
        g_dx.get_debug = (PFN_D3D12_GET_DEBUG_INTERFACE)(void*)GetProcAddress(g_dx.d3d12, "D3D12GetDebugInterface");
        g_dx.create_factory2 = (AedxCreateFactory2)(void*)GetProcAddress(g_dx.dxgi, "CreateDXGIFactory2");
        g_dx.create_factory1 = (AedxCreateFactory1)(void*)GetProcAddress(g_dx.dxgi, "CreateDXGIFactory1");
        if (g_dx.compiler) g_dx.compile = (pD3DCompile)(void*)GetProcAddress(g_dx.compiler, "D3DCompile");
        if (!g_dx.create_device || !g_dx.serialize_root ||
            (!g_dx.create_factory2 && !g_dx.create_factory1)) {
            rc = aedx_fail(AEDX_ERR_NO_LOADER, "the Direct3D 12 runtime is missing an entry point");
        } else {
            g_dx.loaded = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_load_lock);
    return rc;
}

/* The value of a numeric environment switch: 0 when unset or "0". */
static int aedx_env_level(const char* name) {
    char v[8];
    DWORD n = GetEnvironmentVariableA(name, v, (DWORD)sizeof(v));
    if (n == 0 || n >= sizeof(v)) return 0;
    return atoi(v);
}

static int aedx_hr_status(HRESULT hr) {
    if (hr == E_OUTOFMEMORY) return AEDX_ERR_OOM;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG ||
        hr == DXGI_ERROR_DEVICE_RESET) return AEDX_ERR_DEVICE_LOST;
    return AEDX_ERR_UNSUPPORTED;
}

/* ------------------------------------------------------------------------ */
/* Objects                                                                   */
/* ------------------------------------------------------------------------ */

#define AEDX_MAX_BINDINGS 8
#define AEDX_MAX_ATTRS    16
#define AEDX_MAX_DESC     8
#define AEDX_MAX_PUSH     128
#define AEDX_MAX_FRAMES   8
#define AEDX_MAX_COLOR    4   /* colour attachments a target can have (#2386) */
/* Compare ops for sampling a depth (#2373): 1 LESS, 2 LESS_EQUAL, 3 GREATER,
 * 4 GREATER_EQUAL, the shared shape's numbering; slot 0 is unused. */
#define AEDX_COMPARE_OPS  9   /* COMPARE_LESS 1 .. COMPARE_NEVER 8 (#2399) */

/* Shader-visible heaps, one of each per device: every texture's SRV and
 * sampler lives in a slot of these, so any draw can bind any texture without
 * switching heaps (a heap switch is a pipeline flush on some hardware). */
#define AEDX_SRV_SLOTS     4096
#define AEDX_SAMPLER_SLOTS 1024

typedef struct {
    ID3D12DescriptorHeap* heap;
    UINT                  inc;
    int                   capacity;
    int*                  free_list;   /* stack of free slot numbers */
    int                   free_count;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu0;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu0;
} AedxHeap;

struct AedxDevice {
    SRWLOCK             lock;
    IDXGIFactory4*      factory;
    IDXGIAdapter1*      adapter;
    ID3D12Device*       device;
    ID3D12CommandQueue* queue;
    ID3D12Fence*        fence;       /* one timeline for everything submitted */
    UINT64              fence_next;  /* the value the next submission signals */
    ID3D12InfoQueue*    info;        /* debug layer messages, when enabled */
    int                 debug_messages;
    AedxHeap            srv;
    AedxHeap            samplers;
    UINT                rtv_inc, dsv_inc;
    int                 warp;
    int                 allow_tearing;
    /* The present pass (a textured full-screen triangle), made on first use. */
    ID3D12RootSignature* present_root;
    ID3D12PipelineState* present_pso[2];   /* [0] UNORM view, [1] sRGB view */
    /* Indirect draws (#2198): a command signature for each command shape,
     * indexed and not, made on first use. */
    ID3D12CommandSignature* sig_indexed;
    ID3D12CommandSignature* sig_draw;
    /* GPU timestamps (#2198): ticks a second on the queue, 0 when it keeps
     * no time. */
    UINT64              timestamp_freq;
    char                name[256];
};

/* ------------------------------------------------------------------------ */
/* Device                                                                    */
/* ------------------------------------------------------------------------ */

/* Prints what the debug layer recorded since the last drain, and forgets it.
 * Only with AETHER_D3D12_DEBUG=1: without it `info` is NULL. */
static void aedx_drain_debug(AedxDevice* d) {
    if (!d || !d->info) return;
    UINT64 n = ID3D12InfoQueue_GetNumStoredMessages(d->info);
    for (UINT64 i = 0; i < n; i++) {
        SIZE_T len = 0;
        if (FAILED(ID3D12InfoQueue_GetMessage(d->info, i, NULL, &len)) || len == 0) continue;
        D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
        if (!m) break;
        if (SUCCEEDED(ID3D12InfoQueue_GetMessage(d->info, i, m, &len))) {
            const char* sev = m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" :
                              m->Severity == D3D12_MESSAGE_SEVERITY_ERROR ? "ERROR" :
                              m->Severity == D3D12_MESSAGE_SEVERITY_WARNING ? "WARNING" : "INFO";
            fprintf(stderr, "D3D12 %s #%d: %.*s\n", sev, (int)m->ID,
                    (int)m->DescriptionByteLength, m->pDescription);
            /* Counted when they point at a problem; INFO and MESSAGE lines
             * (the layer announcing itself) are printed only. */
            if (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) d->debug_messages++;
        }
        free(m);
    }
    ID3D12InfoQueue_ClearStoredMessages(d->info);
}

int aedx_debug_message_count(const AedxDevice* d) { return d ? d->debug_messages : 0; }

static int aedx_heap_init(AedxDevice* d, AedxHeap* h, D3D12_DESCRIPTOR_HEAP_TYPE type, int capacity) {
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    memset(&hd, 0, sizeof(hd));
    hd.Type = type;
    hd.NumDescriptors = (UINT)capacity;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT hr = ID3D12Device_CreateDescriptorHeap(d->device, &hd, &IID_ID3D12DescriptorHeap,
                                                   (void**)&h->heap);
    if (FAILED(hr)) return aedx_fail(AEDX_ERR_OOM, "CreateDescriptorHeap failed (0x%08lx)", (unsigned long)hr);
    h->inc = ID3D12Device_GetDescriptorHandleIncrementSize(d->device, type);
    h->capacity = capacity;
    h->free_list = (int*)malloc((size_t)capacity * sizeof(int));
    if (!h->free_list) return aedx_fail(AEDX_ERR_OOM, "out of memory");
    /* Highest first on the stack, so slots are handed out from 0 upward. */
    for (int i = 0; i < capacity; i++) h->free_list[i] = capacity - 1 - i;
    h->free_count = capacity;
    h->cpu0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(h->heap);
    h->gpu0 = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(h->heap);
    return AEDX_OK;
}

static void aedx_heap_free_all(AedxHeap* h) {
    if (h->heap) ID3D12DescriptorHeap_Release(h->heap);
    free(h->free_list);
    memset(h, 0, sizeof(*h));
}

/* A slot in a shader-visible heap, or -1 with the reason set when it is
 * full. THE DEVICE LOCK MUST BE HELD. */
static int aedx_slot_take(AedxHeap* h, const char* what) {
    if (h->free_count == 0) {
        aedx_fail(AEDX_ERR_OOM, "all %d %s descriptors are in use", h->capacity, what);
        return -1;
    }
    return h->free_list[--h->free_count];
}

static void aedx_slot_give(AedxHeap* h, int slot) {
    if (slot >= 0 && h->free_count < h->capacity) h->free_list[h->free_count++] = slot;
}

static D3D12_CPU_DESCRIPTOR_HANDLE aedx_slot_cpu(const AedxHeap* h, int slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE c = h->cpu0;
    c.ptr += (SIZE_T)slot * h->inc;
    return c;
}

static D3D12_GPU_DESCRIPTOR_HANDLE aedx_slot_gpu(const AedxHeap* h, int slot) {
    D3D12_GPU_DESCRIPTOR_HANDLE g = h->gpu0;
    g.ptr += (UINT64)slot * h->inc;
    return g;
}

/* AETHER_D3D12_ADAPTER=warp selects WARP even where there is a GPU: the
 * software rasteriser every Windows ships, which is what a machine without
 * a GPU (a CI runner) gets anyway, made reachable on one that has a GPU so
 * the two can be compared. */
static int aedx_want_warp(void) {
    char v[16];
    DWORD n = GetEnvironmentVariableA("AETHER_D3D12_ADAPTER", v, (DWORD)sizeof(v));
    return n > 0 && n < sizeof(v) && _stricmp(v, "warp") == 0;
}

/* The adapter to use: the first hardware one that makes a feature level 11_0
 * device, by the system's high-performance preference when DXGI can rank
 * them; WARP when none does, or when AETHER_D3D12_ADAPTER=warp asks for it.
 * Returns an owned reference. */
static int aedx_pick_adapter(IDXGIFactory4* f, IDXGIAdapter1** out, int* is_warp, char* name, size_t name_len) {
    IDXGIFactory6* f6 = NULL;
    if (aedx_want_warp()) goto warp;
    IDXGIFactory4_QueryInterface(f, &IID_IDXGIFactory6, (void**)&f6);
    for (UINT i = 0;; i++) {
        IDXGIAdapter1* a = NULL;
        HRESULT hr = f6 ? IDXGIFactory6_EnumAdapterByGpuPreference(f6, i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                   &IID_IDXGIAdapter1, (void**)&a)
                        : IDXGIFactory4_EnumAdapters1(f, i, &a);
        if (hr == DXGI_ERROR_NOT_FOUND || !a) break;
        if (FAILED(hr)) break;
        DXGI_ADAPTER_DESC1 desc;
        IDXGIAdapter1_GetDesc1(a, &desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(g_dx.create_device((IUnknown*)a, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, NULL))) {
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, (int)name_len, NULL, NULL);
            *out = a;
            *is_warp = 0;
            if (f6) IDXGIFactory6_Release(f6);
            return AEDX_OK;
        }
        IDXGIAdapter1_Release(a);
    }
    if (f6) IDXGIFactory6_Release(f6);

warp:;
    IDXGIAdapter1* warp = NULL;
    if (SUCCEEDED(IDXGIFactory4_EnumWarpAdapter(f, &IID_IDXGIAdapter1, (void**)&warp)) && warp) {
        if (SUCCEEDED(g_dx.create_device((IUnknown*)warp, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, NULL))) {
            DXGI_ADAPTER_DESC1 desc;
            IDXGIAdapter1_GetDesc1(warp, &desc);
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, (int)name_len, NULL, NULL);
            *out = warp;
            *is_warp = 1;
            return AEDX_OK;
        }
        IDXGIAdapter1_Release(warp);
    }
    return aedx_fail(AEDX_ERR_NO_DEVICE, "no adapter supports Direct3D 12 at feature level 11_0");
}

static int aedx_make_factory(IDXGIFactory4** out, int debug) {
    HRESULT hr;
    if (g_dx.create_factory2) {
        hr = g_dx.create_factory2(debug ? DXGI_CREATE_FACTORY_DEBUG : 0, &IID_IDXGIFactory4, (void**)out);
        /* The DXGI debug flag needs the graphics tools installed; without
         * them the plain factory is the right answer, not a failure. */
        if (FAILED(hr) && debug) hr = g_dx.create_factory2(0, &IID_IDXGIFactory4, (void**)out);
    } else {
        hr = g_dx.create_factory1(&IID_IDXGIFactory4, (void**)out);
    }
    if (FAILED(hr)) return aedx_fail(AEDX_ERR_NO_LOADER, "cannot create a DXGI 1.4 factory (0x%08lx)", (unsigned long)hr);
    return AEDX_OK;
}

static char g_probe_name[256];
static int  g_probe;   /* 0 unprobed, 1 usable, -1 not */
static SRWLOCK g_probe_lock = SRWLOCK_INIT;

int aedx_available(void) {
    if (g_probe) return g_probe > 0;
    AcquireSRWLockExclusive(&g_probe_lock);
    if (!g_probe) {
        g_probe = -1;
        IDXGIFactory4* f = NULL;
        IDXGIAdapter1* a = NULL;
        int warp = 0;
        if (aedx_load() == AEDX_OK && aedx_make_factory(&f, 0) == AEDX_OK &&
            aedx_pick_adapter(f, &a, &warp, g_probe_name, sizeof(g_probe_name)) == AEDX_OK) {
            g_probe = 1;
            aedx_clear_error();
        }
        if (a) IDXGIAdapter1_Release(a);
        if (f) IDXGIFactory4_Release(f);
    }
    int ok = g_probe > 0;
    ReleaseSRWLockExclusive(&g_probe_lock);
    return ok;
}

const char* aedx_device_name(void) {
    return aedx_available() ? g_probe_name : "";
}

AedxDevice* aedx_device_create(void) {
    aedx_clear_error();
    if (aedx_load() != AEDX_OK) return NULL;
    AedxDevice* d = (AedxDevice*)calloc(1, sizeof(*d));
    if (!d) { aedx_fail(AEDX_ERR_OOM, "out of memory"); return NULL; }
    InitializeSRWLock(&d->lock);

    /* AETHER_D3D12_DEBUG=1 enables the debug layer; 2 adds GPU-based
     * validation, which also checks descriptor and resource-state use as the
     * GPU executes, at a large cost in speed. */
    int debug = aedx_env_level("AETHER_D3D12_DEBUG");
    if (debug && g_dx.get_debug) {
        ID3D12Debug* dbg = NULL;
        if (SUCCEEDED(g_dx.get_debug(&IID_ID3D12Debug, (void**)&dbg)) && dbg) {
            ID3D12Debug_EnableDebugLayer(dbg);
            if (debug >= 2) {
                ID3D12Debug1* dbg1 = NULL;
                if (SUCCEEDED(ID3D12Debug_QueryInterface(dbg, &IID_ID3D12Debug1, (void**)&dbg1)) && dbg1) {
                    ID3D12Debug1_SetEnableGPUBasedValidation(dbg1, TRUE);
                    ID3D12Debug1_Release(dbg1);
                }
            }
            ID3D12Debug_Release(dbg);
        }
    }
    if (aedx_make_factory(&d->factory, debug) != AEDX_OK) goto fail;
    if (aedx_pick_adapter(d->factory, &d->adapter, &d->warp, d->name, sizeof(d->name)) != AEDX_OK) goto fail;
    HRESULT hr = g_dx.create_device((IUnknown*)d->adapter, D3D_FEATURE_LEVEL_11_0,
                                    &IID_ID3D12Device, (void**)&d->device);
    if (FAILED(hr)) {
        aedx_fail(AEDX_ERR_NO_DEVICE, "D3D12CreateDevice failed (0x%08lx)", (unsigned long)hr);
        goto fail;
    }
    if (debug && SUCCEEDED(ID3D12Device_QueryInterface(d->device, &IID_ID3D12InfoQueue, (void**)&d->info))) {
        /* One advisory is left out: #820, "no (or a different) clear value
         * was given when the resource was created". Every draw here takes
         * its clear colour as an argument, so a target has no single
         * optimized clear value to declare, and the message would repeat on
         * every frame of every correct program. Everything else is kept. */
        D3D12_MESSAGE_ID deny[1] = { D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE };
        D3D12_INFO_QUEUE_FILTER filter;
        memset(&filter, 0, sizeof(filter));
        filter.DenyList.NumIDs = 1;
        filter.DenyList.pIDList = deny;
        ID3D12InfoQueue_AddStorageFilterEntries(d->info, &filter);
    }

    D3D12_COMMAND_QUEUE_DESC qd;
    memset(&qd, 0, sizeof(qd));
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = ID3D12Device_CreateCommandQueue(d->device, &qd, &IID_ID3D12CommandQueue, (void**)&d->queue);
    if (FAILED(hr)) { aedx_fail(AEDX_ERR_OOM, "CreateCommandQueue failed (0x%08lx)", (unsigned long)hr); goto fail; }
    hr = ID3D12Device_CreateFence(d->device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&d->fence);
    if (FAILED(hr)) { aedx_fail(AEDX_ERR_OOM, "CreateFence failed (0x%08lx)", (unsigned long)hr); goto fail; }
    d->fence_next = 1;
    UINT64 freq = 0;
    if (SUCCEEDED(ID3D12CommandQueue_GetTimestampFrequency(d->queue, &freq))) d->timestamp_freq = freq;

    if (aedx_heap_init(d, &d->srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, AEDX_SRV_SLOTS) != AEDX_OK) goto fail;
    if (aedx_heap_init(d, &d->samplers, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, AEDX_SAMPLER_SLOTS) != AEDX_OK) goto fail;
    d->rtv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(d->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    d->dsv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(d->device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    /* Presenting without vsync on a variable-refresh display needs the
     * swapchain made with tearing allowed, which DXGI 1.5 reports. */
    IDXGIFactory5* f5 = NULL;
    if (SUCCEEDED(IDXGIFactory4_QueryInterface(d->factory, &IID_IDXGIFactory5, (void**)&f5)) && f5) {
        BOOL tearing = FALSE;
        if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                        &tearing, sizeof(tearing)))) {
            d->allow_tearing = tearing ? 1 : 0;
        }
        IDXGIFactory5_Release(f5);
    }
    aedx_drain_debug(d);
    return d;

fail:
    aedx_device_destroy(d);
    return NULL;
}

int aedx_device_is_warp(const AedxDevice* d) { return d ? d->warp : 0; }

/* Waits until the queue has passed `value` on the device timeline, up to
 * `timeout_ms`, using `event` (the caller's own, so waits on different
 * objects never share one). */
static int aedx_wait_value(AedxDevice* d, UINT64 value, HANDLE event, DWORD timeout_ms) {
    if (ID3D12Fence_GetCompletedValue(d->fence) >= value) return AEDX_OK;
    HRESULT hr = ID3D12Fence_SetEventOnCompletion(d->fence, value, event);
    if (FAILED(hr)) return aedx_fail(aedx_hr_status(hr), "SetEventOnCompletion failed (0x%08lx)", (unsigned long)hr);
    DWORD w = WaitForSingleObject(event, timeout_ms);
    if (w == WAIT_TIMEOUT) {
        return aedx_fail(AEDX_ERR_DEVICE_LOST, "the GPU did not finish within %lu ms", (unsigned long)timeout_ms);
    }
    if (w != WAIT_OBJECT_0) return aedx_fail(AEDX_ERR_DEVICE_LOST, "waiting for the GPU failed");
    /* A removed device completes every fence with UINT64_MAX. */
    if (ID3D12Fence_GetCompletedValue(d->fence) == UINT64_MAX) {
        HRESULT why = ID3D12Device_GetDeviceRemovedReason(d->device);
        return aedx_fail(AEDX_ERR_DEVICE_LOST, "the device was removed (0x%08lx)", (unsigned long)why);
    }
    return AEDX_OK;
}

/* Executes one closed command list and signals the next timeline value,
 * which it returns (0 on failure). THE DEVICE LOCK MUST BE HELD, so values
 * are signalled in submission order. */
static UINT64 aedx_execute(AedxDevice* d, ID3D12GraphicsCommandList* list) {
    ID3D12CommandList* lists[1] = { (ID3D12CommandList*)list };
    ID3D12CommandQueue_ExecuteCommandLists(d->queue, 1, lists);
    UINT64 v = d->fence_next++;
    HRESULT hr = ID3D12CommandQueue_Signal(d->queue, d->fence, v);
    aedx_drain_debug(d);
    if (FAILED(hr)) {
        aedx_fail(aedx_hr_status(hr), "Signal failed (0x%08lx)", (unsigned long)hr);
        return 0;
    }
    return v;
}

/* Waits for everything submitted so far. THE DEVICE LOCK MUST BE HELD. */
static int aedx_idle(AedxDevice* d) {
    UINT64 v = d->fence_next++;
    HRESULT hr = ID3D12CommandQueue_Signal(d->queue, d->fence, v);
    if (FAILED(hr)) return aedx_fail(aedx_hr_status(hr), "Signal failed (0x%08lx)", (unsigned long)hr);
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!ev) return aedx_fail(AEDX_ERR_OOM, "CreateEvent failed");
    int rc = aedx_wait_value(d, v, ev, 30000);
    CloseHandle(ev);
    return rc;
}

void aedx_device_destroy(AedxDevice* d) {
    if (!d) return;
    if (d->queue && d->fence) {
        AcquireSRWLockExclusive(&d->lock);
        aedx_idle(d);
        ReleaseSRWLockExclusive(&d->lock);
    }
    aedx_drain_debug(d);
    for (int i = 0; i < 2; i++) {
        if (d->present_pso[i]) ID3D12PipelineState_Release(d->present_pso[i]);
    }
    if (d->present_root) ID3D12RootSignature_Release(d->present_root);
    if (d->sig_indexed) ID3D12CommandSignature_Release(d->sig_indexed);
    if (d->sig_draw) ID3D12CommandSignature_Release(d->sig_draw);
    aedx_heap_free_all(&d->srv);
    aedx_heap_free_all(&d->samplers);
    if (d->fence) ID3D12Fence_Release(d->fence);
    if (d->queue) ID3D12CommandQueue_Release(d->queue);
    if (d->info) ID3D12InfoQueue_Release(d->info);
    if (d->device) ID3D12Device_Release(d->device);
    if (d->adapter) IDXGIAdapter1_Release(d->adapter);
    if (d->factory) IDXGIFactory4_Release(d->factory);
    free(d);
}

/* A command allocator and a closed list on it, for one-shot work (texture
 * uploads) and as the per-frame pair of targets, computes and swapchains. */
static int aedx_make_list(AedxDevice* d, ID3D12CommandAllocator** alloc, ID3D12GraphicsCommandList** list) {
    HRESULT hr = ID3D12Device_CreateCommandAllocator(d->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     &IID_ID3D12CommandAllocator, (void**)alloc);
    if (FAILED(hr)) return aedx_fail(AEDX_ERR_OOM, "CreateCommandAllocator failed (0x%08lx)", (unsigned long)hr);
    hr = ID3D12Device_CreateCommandList(d->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, *alloc, NULL,
                                        &IID_ID3D12GraphicsCommandList, (void**)list);
    if (FAILED(hr)) return aedx_fail(AEDX_ERR_OOM, "CreateCommandList failed (0x%08lx)", (unsigned long)hr);
    ID3D12GraphicsCommandList_Close(*list);
    return AEDX_OK;
}

/* A committed buffer in heap `type`, `bytes` long, in `state`. */
static ID3D12Resource* aedx_make_buffer(AedxDevice* d, D3D12_HEAP_TYPE type, UINT64 bytes,
                                        D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES hp;
    memset(&hp, 0, sizeof(hp));
    hp.Type = type;
    D3D12_RESOURCE_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    ID3D12Resource* r = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(d->device, &hp, D3D12_HEAP_FLAG_NONE, &rd, state,
                                                      NULL, &IID_ID3D12Resource, (void**)&r);
    if (FAILED(hr)) {
        aedx_fail(aedx_hr_status(hr), "CreateCommittedResource (buffer of %llu bytes) failed (0x%08lx)",
                  (unsigned long long)bytes, (unsigned long)hr);
        return NULL;
    }
    return r;
}

static void aedx_barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r,
                         D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

/* One subresource's transition: a level of one layer of a texture. */
static void aedx_barrier_sub(ID3D12GraphicsCommandList* list, ID3D12Resource* r, UINT sub,
                             D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = sub;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

/* ------------------------------------------------------------------------ */
/* Shaders                                                                   */
/* ------------------------------------------------------------------------ */

/* Bytecode for a stage: `src` as it is when it is already a DXBC container
 * (what fxc and dxc emit), otherwise HLSL source compiled for `profile`,
 * entry point main. Returns an owned blob. */
static int aedx_shader(const void* src, size_t len, const char* profile, ID3DBlob** out) {
    if (!src || len == 0) return aedx_fail(AEDX_ERR_SHADER, "the %s shader is empty", profile);
    if (len >= 4 && memcmp(src, "DXBC", 4) == 0) {
        if (!g_dx.compiler) {
            /* D3DCreateBlob lives in the compiler DLL too. */
            return aedx_fail(AEDX_ERR_SHADER, "d3dcompiler_47.dll is missing");
        }
        typedef HRESULT (WINAPI *CreateBlobFn)(SIZE_T, ID3DBlob**);
        CreateBlobFn create_blob = (CreateBlobFn)(void*)GetProcAddress(g_dx.compiler, "D3DCreateBlob");
        if (!create_blob || FAILED(create_blob(len, out))) {
            return aedx_fail(AEDX_ERR_OOM, "cannot hold %zu bytes of bytecode", len);
        }
        memcpy(ID3D10Blob_GetBufferPointer(*out), src, len);
        return AEDX_OK;
    }
    if (!g_dx.compile) {
        return aedx_fail(AEDX_ERR_SHADER,
                         "HLSL source needs d3dcompiler_47.dll, which is missing; pass compiled bytecode instead");
    }
    ID3DBlob* errors = NULL;
    HRESULT hr = g_dx.compile(src, len, profile, NULL, NULL, "main", profile,
                              D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                              0, out, &errors);
    if (FAILED(hr)) {
        const char* msg = errors ? (const char*)ID3D10Blob_GetBufferPointer(errors) : "no message";
        aedx_fail(AEDX_ERR_SHADER, "%s: %s", profile, msg);
        if (errors) ID3D10Blob_Release(errors);
        return AEDX_ERR_SHADER;
    }
    if (errors) ID3D10Blob_Release(errors);
    return AEDX_OK;
}

/* ------------------------------------------------------------------------ */
/* Targets                                                                   */
/* ------------------------------------------------------------------------ */

/* A caller's buffer (#1515): defined here because draws name buffers too,
 * as vertex streams and indirect commands (#2198). */
struct AedxBuffer {
    AedxDevice*     dev;
    ID3D12Resource* res;
    unsigned char*  ptr;
    size_t          size;
};

/* One draw inside a frame. `instances` 0 draws the target's instance count
 * from instance 0; above 0, that many from `first_instance`. An indirect draw
 * reads `indirect_draws` commands from `indirect` at `indirect_offset`, and
 * `offsets` is each dynamic uniform binding's offset, by binding (#2198). */
typedef struct {
    AedxMaterial* mat;
    int           first;
    int           count;
    int           first_instance;
    int           instances;
    AedxBuffer*   indirect;
    UINT64        indirect_offset;
    int           indirect_draws;
    UINT          offsets[AEDX_MAX_DESC];
    /* A scissor and a viewport of the draw's own (#2398), in pixels from
     * the target's top left; unset, the whole target. */
    int           scissor_set;
    int           sx, sy, sw, sh;
    int           viewport_set;
    float         vx, vy, vw, vh, vmin, vmax;
    /* The draw's own stencil reference (#2399); unset, the pipeline's. */
    int           stencil_ref_set;
    UINT          stencil_ref;
    /* A pipeline of the draw's own (#2411), made for the same target; NULL
     * draws with the frame's. */
    AedxPipeline* pipe;
} AedxDrawItem;

/* The bytes of one indirect draw command, indexed and not: the layouts of
 * D3D12_DRAW_INDEXED_ARGUMENTS and D3D12_DRAW_ARGUMENTS. */
#define AEDX_INDIRECT_INDEXED_BYTES 20
#define AEDX_INDIRECT_BYTES         16

typedef struct {
    ID3D12CommandAllocator*    alloc;
    ID3D12GraphicsCommandList* list;
    HANDLE                     event;
    UINT64                     value;       /* timeline value of its last submission */
    int                        submitted;
    ID3D12Resource*            readback;    /* READBACK heap, mapped for its life */
    unsigned char*             readback_ptr;
    /* The same for colour attachments 1.. (#2386). */
    ID3D12Resource*            xreadback[AEDX_MAX_COLOR - 1];
    unsigned char*             xreadback_ptr[AEDX_MAX_COLOR - 1];
    int                        timed;       /* writes the slot's two timestamps (#2198) */
    unsigned long long         seq;         /* the target's submission it holds */
} AedxFrame;

struct AedxTarget {
    AedxDevice*     dev;
    int             width, height;
    DXGI_FORMAT     format;
    int             bpp;
    int             samples;
    int             has_depth;
    /* A stencil with the depth (#2399): D24_UNORM_S8_UINT, as contrib.vulkan
     * picks, where the depth alone is D32_FLOAT. The three formats are the
     * depth view's, the resource's when it is sampled, and the shader
     * view's. */
    int             has_stencil;
    DXGI_FORMAT     dsv_format, depth_typeless, depth_srv_format;
    /* A layered target (#2399): `layers` array slices of colour (a cube's six
     * faces with `cube`), and of depth, a render target and depth view each
     * in `rtv_heap` and `dsv_heap`. Draws render into `layer`; `srv_slot`
     * reads all the slices as a Texture2DArray and `cube_srv_slot` as a
     * TextureCube. */
    int             layered;
    int             layers;
    int             cube;
    int             layer;
    int             cube_srv_slot;
    /* A layered target's depth read as a texture (#2412): `depth_srv_slot`
     * all its slices as a Texture2DArray, this one a cube's as a
     * TextureCube. `layers_fresh` until the first frame has cleared every
     * slice, so one never drawn reads 0 colour and the far plane. */
    int             depth_cube_srv_slot;
    int             layers_fresh;
    ID3D12Resource* color;          /* single-sample: resolved into, read back, presented */
    ID3D12Resource* msaa;
    ID3D12Resource* depth;
    D3D12_RESOURCE_STATES color_state;
    /* Two views an attachment: [2N] attachment N, [2N + 1] its multisampled
     * image. */
    ID3D12DescriptorHeap* rtv_heap;
    /* Colour attachments past the first (#2386): a target_create_mrt
     * target's attachments 1.., each with its format, image (and
     * multisampled image), state, shader view, sampler and readback
     * footprint. Attachment 0 is the fields around them. */
    int             extra;
    DXGI_FORMAT     xformat[AEDX_MAX_COLOR - 1];
    int             xbpp[AEDX_MAX_COLOR - 1];
    ID3D12Resource* xcolor[AEDX_MAX_COLOR - 1];
    ID3D12Resource* xmsaa[AEDX_MAX_COLOR - 1];
    D3D12_RESOURCE_STATES xstate[AEDX_MAX_COLOR - 1];
    int             xsrv_slot[AEDX_MAX_COLOR - 1];
    int             xsampler_slot[AEDX_MAX_COLOR - 1];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT xfootprint[AEDX_MAX_COLOR - 1];
    UINT64          xreadback_bytes[AEDX_MAX_COLOR - 1];
    ID3D12DescriptorHeap* dsv_heap;
    int             srv_slot;       /* the colour image as a texture: presenting, and a later pass */
    /* Sampling the target in a later pass (#2198). `sampler_slot` filters the
     * colour linearly where the format allows and by nearest texel otherwise;
     * `depth_sampler_slot` reads depth by nearest texel. `depth_sampled` is
     * set the first time a material binds the depth: from then on the depth
     * is a typeless resource with a shader view (`depth_srv_slot`), where it
     * was created DENY_SHADER_RESOURCE. */
    int             sampler_slot;
    int             depth_sampler_slot;
    int             depth_srv_slot;
    int             depth_sampled;
    /* Comparison samplers for the depth (#2373), one per compare op 1..4,
     * made when a material first asks; -1 until then. R32_FLOAT comparison
     * filtering is required of every Direct3D 12 device, so they filter
     * linearly. */
    int             compare_slot[AEDX_COMPARE_OPS];
    D3D12_RESOURCE_STATES depth_state;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64          readback_bytes;
    int             readback_on;
    int             rendered;

    ID3D12Resource* vbuf;
    unsigned char*  vbuf_ptr;
    int             vbuf_capacity;  /* floats */
    int             vertex_count;
    int             vertex_floats;
    ID3D12Resource* ibuf;
    unsigned char*  ibuf_ptr;
    int             ibuf_capacity;  /* bytes */
    int             index_count;
    int             index_bits;

    AedxDrawItem*   batch;
    int             batch_count, batch_cap;
    unsigned char   push[AEDX_MAX_PUSH];
    UINT            push_size;

    /* Vertex streams past slot 0, a caller's buffer each, and the instances
     * each draw makes, 1 unless set (#2198). */
    AedxBuffer*     streams[AEDX_MAX_BINDINGS];
    int             instance_count;

    /* GPU timestamps (#2198): two a frame slot, resolved into `timer_rb`
     * (mapped for its life) and read when the slot is waited on. */
    ID3D12QueryHeap* timer_heap;
    ID3D12Resource*  timer_rb;
    UINT64*          timer_ptr;
    int              timing_on;
    double           gpu_ms;
    /* The submission `gpu_ms` came from, so waiting on the slots out of
     * order never reports an older frame. */
    unsigned long long submitted_seq;
    unsigned long long gpu_seq;

    AedxFrame       frames[AEDX_MAX_FRAMES];
    int             frame_count;
    int             next_frame;
    int             last_submitted;
    DWORD           timeout_ms;
};

static int aedx_format_bpp(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return 4;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:  return 8;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:  return 16;
        default:                              return 0;
    }
}

static int aedx_format_srgb_display(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R16G16B16A16_FLOAT ||
           f == DXGI_FORMAT_R32G32B32A32_FLOAT;
}

/* IEEE 754 half to float, subnormals, infinities and NaN included. */
static float aedx_half_to_float(uint16_t h) {
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

static float aedx_channel_value(const AedxTarget* t, const unsigned char* px, int c) {
    switch (t->format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: {
            uint16_t h;
            memcpy(&h, px + c * 2, sizeof(h));
            return aedx_half_to_float(h);
        }
        case DXGI_FORMAT_R32G32B32A32_FLOAT: {
            float f;
            memcpy(&f, px + c * 4, sizeof(f));
            return f;
        }
        default:
            return (float)px[c] / 255.0f;
    }
}

static unsigned char aedx_channel_u8(const AedxTarget* t, const unsigned char* px, int c) {
    if (t->bpp == 4) return px[c];
    float v = aedx_channel_value(t, px, c);
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    return (unsigned char)(v * 255.0f + 0.5f);
}

static void aedx_frames_free(AedxTarget* t) {
    for (int i = 0; i < t->frame_count; i++) {
        AedxFrame* f = &t->frames[i];
        if (f->readback) {
            if (f->readback_ptr) ID3D12Resource_Unmap(f->readback, 0, NULL);
            ID3D12Resource_Release(f->readback);
        }
        for (int x = 0; x < AEDX_MAX_COLOR - 1; x++) {
            if (!f->xreadback[x]) continue;
            if (f->xreadback_ptr[x]) ID3D12Resource_Unmap(f->xreadback[x], 0, NULL);
            ID3D12Resource_Release(f->xreadback[x]);
        }
        if (f->list) ID3D12GraphicsCommandList_Release(f->list);
        if (f->alloc) ID3D12CommandAllocator_Release(f->alloc);
        if (f->event) CloseHandle(f->event);
    }
    memset(t->frames, 0, sizeof(t->frames));
    t->frame_count = 0;
    t->next_frame = 0;
    t->last_submitted = -1;
}

/* THE DEVICE LOCK MUST BE HELD, and the target idle. */
static int aedx_frames_alloc(AedxTarget* t, int count) {
    AedxDevice* d = t->dev;
    for (int i = 0; i < count; i++) {
        AedxFrame* f = &t->frames[i];
        t->frame_count = i + 1;
        if (aedx_make_list(d, &f->alloc, &f->list) != AEDX_OK) goto fail;
        f->event = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!f->event) { aedx_fail(AEDX_ERR_OOM, "CreateEvent failed"); goto fail; }
        if (t->readback_on) {
            f->readback = aedx_make_buffer(d, D3D12_HEAP_TYPE_READBACK, t->readback_bytes,
                                           D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
            if (!f->readback) goto fail;
            /* A readback buffer stays mapped: Map is a round trip that buys
             * nothing for memory the CPU reads every frame. */
            HRESULT hr = ID3D12Resource_Map(f->readback, 0, NULL, (void**)&f->readback_ptr);
            if (FAILED(hr)) { aedx_fail(AEDX_ERR_OOM, "Map (readback) failed (0x%08lx)", (unsigned long)hr); goto fail; }
            for (int x = 0; x < t->extra; x++) {
                f->xreadback[x] = aedx_make_buffer(d, D3D12_HEAP_TYPE_READBACK, t->xreadback_bytes[x],
                                                   D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
                if (!f->xreadback[x]) goto fail;
                hr = ID3D12Resource_Map(f->xreadback[x], 0, NULL, (void**)&f->xreadback_ptr[x]);
                if (FAILED(hr)) { aedx_fail(AEDX_ERR_OOM, "Map (readback) failed (0x%08lx)", (unsigned long)hr); goto fail; }
            }
        }
    }
    t->next_frame = 0;
    t->last_submitted = -1;
    return AEDX_OK;
fail:
    aedx_frames_free(t);
    return AEDX_ERR_OOM;
}

/* The caller guarantees the target is idle; the device lock must NOT be
 * held, since returning the descriptor slot takes it. */
static void aedx_target_free_images(AedxTarget* t) {
    AcquireSRWLockExclusive(&t->dev->lock);
    aedx_slot_give(&t->dev->srv, t->srv_slot);
    aedx_slot_give(&t->dev->srv, t->cube_srv_slot);
    aedx_slot_give(&t->dev->srv, t->depth_cube_srv_slot);
    aedx_slot_give(&t->dev->srv, t->depth_srv_slot);
    for (int x = 0; x < AEDX_MAX_COLOR - 1; x++) {
        aedx_slot_give(&t->dev->srv, t->xsrv_slot[x]);
        t->xsrv_slot[x] = -1;
    }
    ReleaseSRWLockExclusive(&t->dev->lock);
    t->srv_slot = -1;
    t->cube_srv_slot = -1;
    t->depth_cube_srv_slot = -1;
    t->depth_srv_slot = -1;
    for (int x = 0; x < AEDX_MAX_COLOR - 1; x++) {
        if (t->xcolor[x]) ID3D12Resource_Release(t->xcolor[x]);
        if (t->xmsaa[x]) ID3D12Resource_Release(t->xmsaa[x]);
        t->xcolor[x] = t->xmsaa[x] = NULL;
    }
    if (t->color) ID3D12Resource_Release(t->color);
    if (t->msaa) ID3D12Resource_Release(t->msaa);
    if (t->depth) ID3D12Resource_Release(t->depth);
    if (t->rtv_heap) ID3D12DescriptorHeap_Release(t->rtv_heap);
    if (t->dsv_heap) ID3D12DescriptorHeap_Release(t->dsv_heap);
    t->color = t->msaa = t->depth = NULL;
    t->rtv_heap = t->dsv_heap = NULL;
    t->rendered = 0;
}

static ID3D12Resource* aedx_make_texture2d_layers(AedxDevice* d, int w, int h, DXGI_FORMAT fmt, int samples,
                                                  UINT16 mips, UINT16 layers, D3D12_RESOURCE_FLAGS flags,
                                                  D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear);

static ID3D12Resource* aedx_make_texture2d(AedxDevice* d, int w, int h, DXGI_FORMAT fmt, int samples,
                                           UINT16 mips, D3D12_RESOURCE_FLAGS flags,
                                           D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear) {
    return aedx_make_texture2d_layers(d, w, h, fmt, samples, mips, 1, flags, state, clear);
}

/* A 2D texture of `layers` array slices. */
static ID3D12Resource* aedx_make_texture2d_layers(AedxDevice* d, int w, int h, DXGI_FORMAT fmt, int samples,
                                                  UINT16 mips, UINT16 layers, D3D12_RESOURCE_FLAGS flags,
                                                  D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear) {
    D3D12_HEAP_PROPERTIES hp;
    memset(&hp, 0, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)w;
    rd.Height = (UINT)h;
    rd.DepthOrArraySize = layers;
    rd.MipLevels = mips;
    rd.Format = fmt;
    rd.SampleDesc.Count = (UINT)samples;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = flags;
    ID3D12Resource* r = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(d->device, &hp, D3D12_HEAP_FLAG_NONE, &rd, state,
                                                      clear, &IID_ID3D12Resource, (void**)&r);
    if (FAILED(hr)) {
        aedx_fail(aedx_hr_status(hr), "CreateCommittedResource (%dx%d texture, format %d) failed (0x%08lx)",
                  w, h, (int)fmt, (unsigned long)hr);
        return NULL;
    }
    return r;
}

/* Everything that depends on the size. The caller guarantees the target is
 * idle. */
/* A layered target's images (#2399): colour and depth with `layers` array
 * slices, a render target and depth view a slice, and the views sampling
 * reads them all through. */
static int aedx_target_make_layered(AedxTarget* t) {
    AedxDevice* d = t->dev;
    UINT16 n = (UINT16)t->layers;
    t->color = aedx_make_texture2d_layers(d, t->width, t->height, t->format, 1, 1, n,
                                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                          D3D12_RESOURCE_STATE_COPY_SOURCE, NULL);
    if (!t->color) return AEDX_ERR_OOM;
    t->color_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    memset(&hd, 0, sizeof(hd));
    /* A view a slice, and after them the multisampled image's (#2412). */
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = n + (t->samples > 1 ? 1u : 0u);
    HRESULT hr = ID3D12Device_CreateDescriptorHeap(d->device, &hd, &IID_ID3D12DescriptorHeap, (void**)&t->rtv_heap);
    if (FAILED(hr)) return aedx_fail(AEDX_ERR_OOM, "CreateDescriptorHeap (RTV) failed (0x%08lx)", (unsigned long)hr);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(t->rtv_heap);
    for (UINT l = 0; l < n; l++) {
        D3D12_RENDER_TARGET_VIEW_DESC rv;
        memset(&rv, 0, sizeof(rv));
        rv.Format = t->format;
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rv.Texture2DArray.FirstArraySlice = l;
        rv.Texture2DArray.ArraySize = 1;
        ID3D12Device_CreateRenderTargetView(d->device, t->color, &rv, rtv);
        rtv.ptr += d->rtv_inc;
    }
    if (t->samples > 1) {
        /* Multisampled (#2412): one image each frame renders into and
         * resolves into the slice drawn, and a depth to match. */
        t->msaa = aedx_make_texture2d(d, t->width, t->height, t->format, t->samples, 1,
                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                      D3D12_RESOURCE_STATE_RENDER_TARGET, NULL);
        if (!t->msaa) return AEDX_ERR_OOM;
        ID3D12Device_CreateRenderTargetView(d->device, t->msaa, NULL, rtv);
    }
    if (t->has_depth) {
        D3D12_CLEAR_VALUE cv;
        memset(&cv, 0, sizeof(cv));
        cv.Format = t->dsv_format;
        cv.DepthStencil.Depth = 1.0f;
        int ms = t->samples > 1;
        /* Typeless when a later pass samples it (#2412), as a plain
         * target's depth is. */
        t->depth = aedx_make_texture2d_layers(d, t->width, t->height,
                                              t->depth_sampled ? t->depth_typeless : t->dsv_format,
                                              t->samples, 1, ms ? 1 : n,
                                              D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                              (t->depth_sampled ? D3D12_RESOURCE_FLAG_NONE
                                                                : D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE),
                                              D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv);
        if (!t->depth) return AEDX_ERR_OOM;
        t->depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = ms ? 1 : n;
        hr = ID3D12Device_CreateDescriptorHeap(d->device, &hd, &IID_ID3D12DescriptorHeap, (void**)&t->dsv_heap);
        if (FAILED(hr)) return aedx_fail(AEDX_ERR_OOM, "CreateDescriptorHeap (DSV) failed (0x%08lx)", (unsigned long)hr);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(t->dsv_heap);
        for (UINT l = 0; l < (ms ? 1u : n); l++) {
            D3D12_DEPTH_STENCIL_VIEW_DESC dvd;
            memset(&dvd, 0, sizeof(dvd));
            dvd.Format = t->dsv_format;
            if (ms) {
                dvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
            } else {
                dvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dvd.Texture2DArray.FirstArraySlice = l;
                dvd.Texture2DArray.ArraySize = 1;
            }
            ID3D12Device_CreateDepthStencilView(d->device, t->depth, &dvd, dsv);
            dsv.ptr += d->dsv_inc;
        }
        if (t->depth_sampled) {
            AcquireSRWLockExclusive(&d->lock);
            t->depth_srv_slot = aedx_slot_take(&d->srv, "shader-visible");
            if (t->cube && t->depth_srv_slot >= 0) t->depth_cube_srv_slot = aedx_slot_take(&d->srv, "shader-visible");
            ReleaseSRWLockExclusive(&d->lock);
            if (t->depth_srv_slot < 0 || (t->cube && t->depth_cube_srv_slot < 0)) return AEDX_ERR_OOM;
            D3D12_SHADER_RESOURCE_VIEW_DESC dsv_srv;
            memset(&dsv_srv, 0, sizeof(dsv_srv));
            dsv_srv.Format = t->depth_srv_format;
            dsv_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            dsv_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            dsv_srv.Texture2DArray.MipLevels = 1;
            dsv_srv.Texture2DArray.ArraySize = n;
            ID3D12Device_CreateShaderResourceView(d->device, t->depth, &dsv_srv,
                                                  aedx_slot_cpu(&d->srv, t->depth_srv_slot));
            if (t->cube) {
                memset(&dsv_srv.Texture2DArray, 0, sizeof(dsv_srv.Texture2DArray));
                dsv_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                dsv_srv.TextureCube.MipLevels = 1;
                ID3D12Device_CreateShaderResourceView(d->device, t->depth, &dsv_srv,
                                                      aedx_slot_cpu(&d->srv, t->depth_cube_srv_slot));
            }
        }
    }
    t->layers_fresh = 1;
    AcquireSRWLockExclusive(&d->lock);
    t->srv_slot = aedx_slot_take(&d->srv, "shader-visible");
    if (t->cube && t->srv_slot >= 0) t->cube_srv_slot = aedx_slot_take(&d->srv, "shader-visible");
    ReleaseSRWLockExclusive(&d->lock);
    if (t->srv_slot < 0 || (t->cube && t->cube_srv_slot < 0)) return AEDX_ERR_OOM;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv;
    memset(&sv, 0, sizeof(sv));
    sv.Format = t->format;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Texture2DArray.MipLevels = 1;
    sv.Texture2DArray.ArraySize = n;
    ID3D12Device_CreateShaderResourceView(d->device, t->color, &sv, aedx_slot_cpu(&d->srv, t->srv_slot));
    if (t->cube) {
        memset(&sv.Texture2DArray, 0, sizeof(sv.Texture2DArray));
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = 1;
        ID3D12Device_CreateShaderResourceView(d->device, t->color, &sv, aedx_slot_cpu(&d->srv, t->cube_srv_slot));
    }
    /* Every slice has the same footprint; the readback copies `layer`'s. */
    D3D12_RESOURCE_DESC desc = ID3D12Resource_GetDesc(t->color);
    UINT rows = 0;
    UINT64 row_bytes = 0;
    ID3D12Device_GetCopyableFootprints(d->device, &desc, 0, 1, 0, &t->footprint, &rows, &row_bytes,
                                       &t->readback_bytes);
    return AEDX_OK;
}

static int aedx_target_make_images(AedxTarget* t) {
    AedxDevice* d = t->dev;
    if (t->layered) return aedx_target_make_layered(t);
    t->color = aedx_make_texture2d(d, t->width, t->height, t->format, 1, 1,
                                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE, NULL);
    if (!t->color) return AEDX_ERR_OOM;
    t->color_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (t->samples > 1) {
        t->msaa = aedx_make_texture2d(d, t->width, t->height, t->format, t->samples, 1,
                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                      D3D12_RESOURCE_STATE_RENDER_TARGET, NULL);
        if (!t->msaa) return AEDX_ERR_OOM;
    }
    for (int x = 0; x < t->extra; x++) {
        t->xcolor[x] = aedx_make_texture2d(d, t->width, t->height, t->xformat[x], 1, 1,
                                           D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                           D3D12_RESOURCE_STATE_COPY_SOURCE, NULL);
        if (!t->xcolor[x]) return AEDX_ERR_OOM;
        t->xstate[x] = D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (t->samples > 1) {
            t->xmsaa[x] = aedx_make_texture2d(d, t->width, t->height, t->xformat[x], t->samples, 1,
                                              D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET, NULL);
            if (!t->xmsaa[x]) return AEDX_ERR_OOM;
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    memset(&hd, 0, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 2u * (UINT)(1 + t->extra);
    HRESULT hr = ID3D12Device_CreateDescriptorHeap(d->device, &hd, &IID_ID3D12DescriptorHeap, (void**)&t->rtv_heap);
    if (FAILED(hr)) return aedx_fail(AEDX_ERR_OOM, "CreateDescriptorHeap (RTV) failed (0x%08lx)", (unsigned long)hr);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(t->rtv_heap);
    for (int c = 0; c <= t->extra; c++) {
        ID3D12Resource* single = c == 0 ? t->color : t->xcolor[c - 1];
        ID3D12Resource* multi = c == 0 ? t->msaa : t->xmsaa[c - 1];
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv0;
        rtv.ptr += (SIZE_T)(2 * c) * d->rtv_inc;
        ID3D12Device_CreateRenderTargetView(d->device, single, NULL, rtv);
        if (multi) {
            rtv.ptr += d->rtv_inc;
            ID3D12Device_CreateRenderTargetView(d->device, multi, NULL, rtv);
        }
    }
    if (t->has_depth) {
        D3D12_CLEAR_VALUE cv;
        memset(&cv, 0, sizeof(cv));
        cv.Format = t->dsv_format;
        cv.DepthStencil.Depth = 1.0f;
        /* A depth a later pass samples (#2198) is typeless, so it can have a
         * depth view and a float shader view; one nobody reads denies shader
         * access, which lets the driver keep it compressed. */
        t->depth = aedx_make_texture2d(d, t->width, t->height,
                                       t->depth_sampled ? t->depth_typeless : t->dsv_format,
                                       t->samples, 1,
                                       D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                       (t->depth_sampled ? D3D12_RESOURCE_FLAG_NONE
                                                         : D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE),
                                       D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv);
        if (!t->depth) return AEDX_ERR_OOM;
        t->depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 1;
        hr = ID3D12Device_CreateDescriptorHeap(d->device, &hd, &IID_ID3D12DescriptorHeap, (void**)&t->dsv_heap);
        if (FAILED(hr)) return aedx_fail(AEDX_ERR_OOM, "CreateDescriptorHeap (DSV) failed (0x%08lx)", (unsigned long)hr);
        D3D12_DEPTH_STENCIL_VIEW_DESC dvd;
        memset(&dvd, 0, sizeof(dvd));
        dvd.Format = t->dsv_format;
        dvd.ViewDimension = t->samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
        ID3D12Device_CreateDepthStencilView(d->device, t->depth, &dvd,
                                            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(t->dsv_heap));
        if (t->depth_sampled) {
            AcquireSRWLockExclusive(&d->lock);
            t->depth_srv_slot = aedx_slot_take(&d->srv, "shader-visible");
            ReleaseSRWLockExclusive(&d->lock);
            if (t->depth_srv_slot < 0) return AEDX_ERR_OOM;
            D3D12_SHADER_RESOURCE_VIEW_DESC sv;
            memset(&sv, 0, sizeof(sv));
            sv.Format = t->depth_srv_format;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels = 1;
            ID3D12Device_CreateShaderResourceView(d->device, t->depth, &sv,
                                                  aedx_slot_cpu(&d->srv, t->depth_srv_slot));
        }
    }
    /* The colour image as a texture, which is how presenting reads it. */
    AcquireSRWLockExclusive(&d->lock);
    t->srv_slot = aedx_slot_take(&d->srv, "shader-visible");
    ReleaseSRWLockExclusive(&d->lock);
    if (t->srv_slot < 0) return AEDX_ERR_OOM;
    ID3D12Device_CreateShaderResourceView(d->device, t->color, NULL, aedx_slot_cpu(&d->srv, t->srv_slot));

    D3D12_RESOURCE_DESC desc = ID3D12Resource_GetDesc(t->color);
    UINT rows = 0;
    UINT64 row_bytes = 0;
    ID3D12Device_GetCopyableFootprints(d->device, &desc, 0, 1, 0, &t->footprint, &rows, &row_bytes,
                                       &t->readback_bytes);
    /* Each further attachment as a texture, and its readback footprint. */
    for (int x = 0; x < t->extra; x++) {
        AcquireSRWLockExclusive(&d->lock);
        t->xsrv_slot[x] = aedx_slot_take(&d->srv, "shader-visible");
        ReleaseSRWLockExclusive(&d->lock);
        if (t->xsrv_slot[x] < 0) return AEDX_ERR_OOM;
        ID3D12Device_CreateShaderResourceView(d->device, t->xcolor[x], NULL,
                                              aedx_slot_cpu(&d->srv, t->xsrv_slot[x]));
        D3D12_RESOURCE_DESC xd = ID3D12Resource_GetDesc(t->xcolor[x]);
        ID3D12Device_GetCopyableFootprints(d->device, &xd, 0, 1, 0, &t->xfootprint[x], &rows, &row_bytes,
                                           &t->xreadback_bytes[x]);
    }
    return AEDX_OK;
}

static int aedx_check_size(AedxDevice* d, int width, int height, int bpp) {
    (void)d;
    if (width <= 0 || height <= 0) {
        return aedx_fail(AEDX_ERR_ARG, "size must be positive, got %dx%d", width, height);
    }
    if (width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        return aedx_fail(AEDX_ERR_UNSUPPORTED, "%dx%d exceeds the Direct3D 12 limit of %d",
                         width, height, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
    }
    if ((uint64_t)width * (uint64_t)height * (uint64_t)bpp > 0x7fffffffull) {
        return aedx_fail(AEDX_ERR_UNSUPPORTED, "%dx%d does not fit a readback", width, height);
    }
    return AEDX_OK;
}

/* The shared shape's compare ops, COMPARE_LESS 1 .. COMPARE_NEVER 8. */
static D3D12_COMPARISON_FUNC aedx_compare_func(int op) {
    switch (op) {
        case 1:  return D3D12_COMPARISON_FUNC_LESS;
        case 2:  return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case 3:  return D3D12_COMPARISON_FUNC_GREATER;
        case 4:  return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case 5:  return D3D12_COMPARISON_FUNC_EQUAL;
        case 6:  return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case 7:  return D3D12_COMPARISON_FUNC_ALWAYS;
        default: return D3D12_COMPARISON_FUNC_NEVER;
    }
}

/* A clamped sampler in a slot of the device's sampler heap, filtering
 * linearly or by nearest texel; with `compare` 1..4 a comparison sampler,
 * which returns the fraction of the footprint whose depth passes
 * `reference op texel` (#2373). The device lock must NOT be held. */
static int aedx_make_sampler_ex(AedxDevice* d, int linear, int compare, int* out_slot) {
    AcquireSRWLockExclusive(&d->lock);
    int slot = aedx_slot_take(&d->samplers, "sampler");
    ReleaseSRWLockExclusive(&d->lock);
    if (slot < 0) return AEDX_ERR_OOM;
    D3D12_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof(sd));
    if (compare) {
        sd.Filter = linear ? D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                           : D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        sd.ComparisonFunc = aedx_compare_func(compare);
    } else {
        sd.Filter = linear ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    }
    sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sd.MaxLOD = D3D12_FLOAT32_MAX;
    sd.MaxAnisotropy = 1;
    ID3D12Device_CreateSampler(d->device, &sd, aedx_slot_cpu(&d->samplers, slot));
    *out_slot = slot;
    return AEDX_OK;
}

static int aedx_make_sampler(AedxDevice* d, int linear, int* out_slot) {
    return aedx_make_sampler_ex(d, linear, 0, out_slot);
}

AedxTarget* aedx_target_create(AedxDevice* d, int width, int height) {
    return aedx_target_create_format(d, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
}

AedxTarget* aedx_target_create_ex(AedxDevice* d, int width, int height, int want_depth, int samples) {
    return aedx_target_create_format(d, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, want_depth, samples);
}

static AedxTarget* aedx_target_create_mrt_impl(AedxDevice* d, int width, int height, int count,
                                               const int* formats, int want_depth, int samples,
                                               int layers, int cube);

AedxTarget* aedx_target_create_format(AedxDevice* d, int width, int height, int format,
                                      int want_depth, int samples) {
    return aedx_target_create_mrt_impl(d, width, height, 1, &format, want_depth, samples, 0, 0);
}

/* A target of `layers` array slices (#2399), as contrib.vulkan's. */
AedxTarget* aedx_target_create_layered(AedxDevice* d, int width, int height, int layers, int format,
                                       int want_depth, int cube) {
    return aedx_target_create_layered_ex(d, width, height, layers, format, want_depth, cube, 1);
}

/* A layered target multisampled `samples` times (#2412). */
AedxTarget* aedx_target_create_layered_ex(AedxDevice* d, int width, int height, int layers, int format,
                                          int want_depth, int cube, int samples) {
    if (layers < 1) {
        aedx_clear_error();
        aedx_fail(AEDX_ERR_ARG, "a layered target has at least one layer, not %d", layers);
        return NULL;
    }
    return aedx_target_create_mrt_impl(d, width, height, 1, &format, want_depth, samples, layers, cube ? 1 : 0);
}

/* A target with `count` colour attachments, 1..4, each in its own format
 * (#2386); SV_TargetN writes attachment N. */
AedxTarget* aedx_target_create_mrt(AedxDevice* d, int width, int height, int count, int f0, int f1,
                                   int f2, int f3, int want_depth, int samples) {
    int formats[AEDX_MAX_COLOR] = { f0, f1, f2, f3 };
    if (count < 1 || count > AEDX_MAX_COLOR) {
        aedx_clear_error();
        aedx_fail(AEDX_ERR_ARG, "a target has 1..%d colour attachments, not %d", AEDX_MAX_COLOR, count);
        return NULL;
    }
    return aedx_target_create_mrt_impl(d, width, height, count, formats, want_depth, samples, 0, 0);
}

/* The format checks a colour attachment needs: a target format the device
 * renders to, at the sample count asked for. */
static int aedx_check_color_format(AedxDevice* d, int format, int samples, D3D12_FEATURE_DATA_FORMAT_SUPPORT* fs) {
    if (!aedx_format_bpp((DXGI_FORMAT)format)) {
        return aedx_fail(AEDX_ERR_ARG,
                         "format %d is not a target format (R8G8B8A8_UNORM 28, R8G8B8A8_UNORM_SRGB 29, "
                         "R16G16B16A16_FLOAT 10, R32G32B32A32_FLOAT 2)", format);
    }
    memset(fs, 0, sizeof(*fs));
    fs->Format = (DXGI_FORMAT)format;
    if (FAILED(ID3D12Device_CheckFeatureSupport(d->device, D3D12_FEATURE_FORMAT_SUPPORT, fs, sizeof(*fs))) ||
        !(fs->Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET)) {
        return aedx_fail(AEDX_ERR_UNSUPPORTED, "the device cannot render to format %d", format);
    }
    if (samples > 1) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS ql;
        memset(&ql, 0, sizeof(ql));
        ql.Format = (DXGI_FORMAT)format;
        ql.SampleCount = (UINT)samples;
        if (FAILED(ID3D12Device_CheckFeatureSupport(d->device, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                                    &ql, sizeof(ql))) || ql.NumQualityLevels == 0) {
            return aedx_fail(AEDX_ERR_UNSUPPORTED, "the device does not support %dx multisampling for format %d",
                             samples, format);
        }
    }
    return AEDX_OK;
}

static AedxTarget* aedx_target_create_mrt_impl(AedxDevice* d, int width, int height, int count,
                                               const int* formats, int want_depth, int samples,
                                               int layers, int cube) {
    int format = formats[0];
    aedx_clear_error();
    if (!d) { aedx_fail(AEDX_ERR_ARG, "device is null"); return NULL; }
    if (want_depth < 0 || want_depth > 2) {
        aedx_fail(AEDX_ERR_ARG, "depth is 0 (none), 1 (DEPTH) or 2 (DEPTH_STENCIL), not %d", want_depth);
        return NULL;
    }
    if (layers) {
        if (cube && layers != 6) {
            aedx_fail(AEDX_ERR_ARG, "a cube target has 6 layers, not %d", layers);
            return NULL;
        }
        if (cube && width != height) {
            aedx_fail(AEDX_ERR_ARG, "a cube target's faces are square, not %dx%d", width, height);
            return NULL;
        }
        if (layers > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) {
            aedx_fail(AEDX_ERR_UNSUPPORTED, "a layered target has 1..%d layers, not %d",
                      D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION, layers);
            return NULL;
        }
    }
    DXGI_FORMAT dsv_format = want_depth == 2 ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_D32_FLOAT;
    int bpp = aedx_format_bpp((DXGI_FORMAT)format);
    if (!bpp) {
        aedx_fail(AEDX_ERR_ARG,
                  "format %d is not a target format (R8G8B8A8_UNORM 28, R8G8B8A8_UNORM_SRGB 29, "
                  "R16G16B16A16_FLOAT 10, R32G32B32A32_FLOAT 2)", format);
        return NULL;
    }
    if (aedx_check_size(d, width, height, bpp) != AEDX_OK) return NULL;
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8 && samples != 16) {
        aedx_fail(AEDX_ERR_ARG, "sample count must be 1, 2, 4, 8 or 16 (got %d)", samples);
        return NULL;
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs;
    memset(&fs, 0, sizeof(fs));
    fs.Format = (DXGI_FORMAT)format;
    if (FAILED(ID3D12Device_CheckFeatureSupport(d->device, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) ||
        !(fs.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET)) {
        aedx_fail(AEDX_ERR_UNSUPPORTED, "the device cannot render to format %d", format);
        return NULL;
    }
    if (samples > 1) {
        /* Checked, not rounded down: asking for 8x on hardware with 4x is an
         * error worth naming. Depth is checked at the same count. */
        DXGI_FORMAT check[2] = { (DXGI_FORMAT)format, dsv_format };
        for (int i = 0; i < (want_depth ? 2 : 1); i++) {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS ql;
            memset(&ql, 0, sizeof(ql));
            ql.Format = check[i];
            ql.SampleCount = (UINT)samples;
            if (FAILED(ID3D12Device_CheckFeatureSupport(d->device, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                                        &ql, sizeof(ql))) || ql.NumQualityLevels == 0) {
                aedx_fail(AEDX_ERR_UNSUPPORTED, "the device does not support %dx multisampling for format %d",
                          samples, (int)check[i]);
                return NULL;
            }
        }
    }

    AedxTarget* t = (AedxTarget*)calloc(1, sizeof(*t));
    if (!t) { aedx_fail(AEDX_ERR_OOM, "out of memory"); return NULL; }
    t->dev = d;
    t->width = width;
    t->height = height;
    t->format = (DXGI_FORMAT)format;
    t->bpp = bpp;
    t->samples = samples;
    t->has_depth = want_depth ? 1 : 0;
    t->has_stencil = want_depth == 2;
    t->dsv_format = dsv_format;
    t->depth_typeless = t->has_stencil ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
    t->depth_srv_format = t->has_stencil ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : DXGI_FORMAT_R32_FLOAT;
    t->layered = layers ? 1 : 0;
    t->layers = layers ? layers : 1;
    t->cube = cube;
    t->cube_srv_slot = -1;
    t->depth_cube_srv_slot = -1;
    t->srv_slot = -1;
    t->sampler_slot = -1;
    t->depth_sampler_slot = -1;
    for (int c = 0; c < AEDX_COMPARE_OPS; c++) t->compare_slot[c] = -1;
    t->depth_srv_slot = -1;
    for (int x = 0; x < AEDX_MAX_COLOR - 1; x++) {
        t->xsrv_slot[x] = -1;
        t->xsampler_slot[x] = -1;
    }
    t->readback_on = 1;
    t->index_bits = 32;
    t->timeout_ms = 5000;
    t->instance_count = 1;
    t->gpu_ms = -1.0;
    t->last_submitted = -1;
    /* The samplers a later pass reads the target through (#2198): linear
     * where the format can be filtered, nearest for the depth. */
    int filter_ok = (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0;
    if (aedx_make_sampler(d, filter_ok, &t->sampler_slot) != AEDX_OK) goto fail;
    /* Attachments 1.. (#2386), each checked like the first. */
    t->extra = count - 1;
    for (int x = 0; x < t->extra; x++) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT xfs;
        if (aedx_check_color_format(d, formats[x + 1], samples, &xfs) != AEDX_OK) goto fail;
        t->xformat[x] = (DXGI_FORMAT)formats[x + 1];
        t->xbpp[x] = aedx_format_bpp(t->xformat[x]);
        if (aedx_make_sampler(d, (xfs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0,
                              &t->xsampler_slot[x]) != AEDX_OK) goto fail;
    }
    if (want_depth && aedx_make_sampler(d, 0, &t->depth_sampler_slot) != AEDX_OK) goto fail;
    if (aedx_target_make_images(t) != AEDX_OK) goto fail;
    AcquireSRWLockExclusive(&d->lock);
    int rc = aedx_frames_alloc(t, 1);
    ReleaseSRWLockExclusive(&d->lock);
    if (rc != AEDX_OK) goto fail;
    return t;
fail:
    aedx_target_destroy(t);
    return NULL;
}

/* A timestamp query heap of `count` and a mapped readback buffer it
 * resolves into (#2198). */
static int aedx_make_timer(AedxDevice* d, UINT count, ID3D12QueryHeap** heap, ID3D12Resource** rb,
                           UINT64** ptr) {
    if (!d->timestamp_freq) return aedx_fail(AEDX_ERR_UNSUPPORTED, "the device's queue keeps no timestamps");
    D3D12_QUERY_HEAP_DESC qd;
    memset(&qd, 0, sizeof(qd));
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = count;
    HRESULT hr = ID3D12Device_CreateQueryHeap(d->device, &qd, &IID_ID3D12QueryHeap, (void**)heap);
    if (FAILED(hr)) return aedx_fail(aedx_hr_status(hr), "CreateQueryHeap failed (0x%08lx)", (unsigned long)hr);
    *rb = aedx_make_buffer(d, D3D12_HEAP_TYPE_READBACK, (UINT64)count * sizeof(UINT64),
                           D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    if (!*rb) {
        ID3D12QueryHeap_Release(*heap);
        *heap = NULL;
        return AEDX_ERR_OOM;
    }
    hr = ID3D12Resource_Map(*rb, 0, NULL, (void**)ptr);
    if (FAILED(hr)) {
        ID3D12Resource_Release(*rb);
        ID3D12QueryHeap_Release(*heap);
        *rb = NULL;
        *heap = NULL;
        return aedx_fail(AEDX_ERR_OOM, "Map (timestamps) failed (0x%08lx)", (unsigned long)hr);
    }
    return AEDX_OK;
}

static void aedx_free_timer(ID3D12QueryHeap** heap, ID3D12Resource** rb) {
    if (*rb) {
        ID3D12Resource_Unmap(*rb, 0, NULL);
        ID3D12Resource_Release(*rb);
        *rb = NULL;
    }
    if (*heap) {
        ID3D12QueryHeap_Release(*heap);
        *heap = NULL;
    }
}

/* Milliseconds between the two timestamps at `ts`, which the work just
 * waited for resolved. */
static double aedx_timer_ms(const AedxDevice* d, const UINT64* ts) {
    if (!d->timestamp_freq || ts[1] < ts[0]) return -1.0;
    return (double)(ts[1] - ts[0]) * 1000.0 / (double)d->timestamp_freq;
}

/* Waits for one slot's last submission. THE DEVICE LOCK IS HELD. */
static int aedx_wait_frame(AedxTarget* t, int slot) {
    AedxFrame* f = &t->frames[slot];
    if (!f->submitted) return AEDX_OK;
    int rc = aedx_wait_value(t->dev, f->value, f->event, t->timeout_ms);
    if (rc == AEDX_OK) {
        f->submitted = 0;
        if (f->timed && t->timer_ptr && f->seq > t->gpu_seq) {
            t->gpu_ms = aedx_timer_ms(t->dev, t->timer_ptr + slot * 2);
            t->gpu_seq = f->seq;
        }
    }
    return rc;
}

static int aedx_wait_all_locked(AedxTarget* t) {
    int rc = AEDX_OK;
    for (int i = 0; i < t->frame_count; i++) {
        int one = aedx_wait_frame(t, i);
        if (one != AEDX_OK) rc = one;
    }
    return rc;
}

void aedx_target_destroy(AedxTarget* t) {
    if (!t) return;
    AedxDevice* d = t->dev;
    AcquireSRWLockExclusive(&d->lock);
    aedx_wait_all_locked(t);
    aedx_frames_free(t);
    ReleaseSRWLockExclusive(&d->lock);
    aedx_target_free_images(t);
    AcquireSRWLockExclusive(&d->lock);
    aedx_slot_give(&d->samplers, t->sampler_slot);
    aedx_slot_give(&d->samplers, t->depth_sampler_slot);
    for (int x = 0; x < AEDX_MAX_COLOR - 1; x++) aedx_slot_give(&d->samplers, t->xsampler_slot[x]);
    for (int c = 0; c < AEDX_COMPARE_OPS; c++) aedx_slot_give(&d->samplers, t->compare_slot[c]);
    ReleaseSRWLockExclusive(&d->lock);
    aedx_free_timer(&t->timer_heap, &t->timer_rb);
    if (t->vbuf) ID3D12Resource_Release(t->vbuf);
    if (t->ibuf) ID3D12Resource_Release(t->ibuf);
    free(t->batch);
    free(t);
}

int aedx_target_width(const AedxTarget* t)  { return t ? t->width : 0; }
int aedx_target_height(const AedxTarget* t) { return t ? t->height : 0; }
int aedx_target_format(const AedxTarget* t) { return t ? (int)t->format : 0; }
int aedx_target_bytes_per_pixel(const AedxTarget* t) { return t ? t->bpp : 0; }
int aedx_target_has_depth(const AedxTarget* t) { return t ? t->has_depth : 0; }
int aedx_target_samples(const AedxTarget* t) { return t ? t->samples : 0; }
int aedx_target_readback(const AedxTarget* t) { return t ? t->readback_on : 0; }
int aedx_target_frames(const AedxTarget* t) { return t ? t->frame_count : 0; }
size_t aedx_rgba_size(const AedxTarget* t) {
    return t ? (size_t)t->width * (size_t)t->height * (size_t)t->bpp : 0;
}

int aedx_target_resize(AedxTarget* t, int width, int height) {
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    int rc = aedx_check_size(t->dev, width, height, t->bpp);
    if (rc != AEDX_OK) return rc;
    if (width == t->width && height == t->height && t->color) return AEDX_OK;
    if (t->cube && width != height) {
        return aedx_fail(AEDX_ERR_ARG, "a cube target's faces are square, not %dx%d", width, height);
    }
    AedxDevice* d = t->dev;
    AcquireSRWLockExclusive(&d->lock);
    aedx_wait_all_locked(t);
    int frames = t->frame_count > 0 ? t->frame_count : 1;
    aedx_frames_free(t);
    ReleaseSRWLockExclusive(&d->lock);
    aedx_target_free_images(t);
    t->width = width;
    t->height = height;
    rc = aedx_target_make_images(t);
    if (rc == AEDX_OK) {
        AcquireSRWLockExclusive(&d->lock);
        rc = aedx_frames_alloc(t, frames);
        ReleaseSRWLockExclusive(&d->lock);
    }
    if (rc != AEDX_OK) aedx_target_free_images(t);
    return rc;
}

int aedx_target_set_readback(AedxTarget* t, int on) {
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    on = on ? 1 : 0;
    if (on == t->readback_on) return AEDX_OK;
    AedxDevice* d = t->dev;
    AcquireSRWLockExclusive(&d->lock);
    aedx_wait_all_locked(t);
    int frames = t->frame_count > 0 ? t->frame_count : 1;
    aedx_frames_free(t);
    t->readback_on = on;
    int rc = aedx_frames_alloc(t, frames);
    ReleaseSRWLockExclusive(&d->lock);
    return rc;
}

int aedx_target_set_frames(AedxTarget* t, int count) {
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    if (count < 1 || count > AEDX_MAX_FRAMES) {
        return aedx_fail(AEDX_ERR_ARG, "frames in flight must be 1..%d (got %d)", AEDX_MAX_FRAMES, count);
    }
    if (count == t->frame_count) return AEDX_OK;
    AedxDevice* d = t->dev;
    AcquireSRWLockExclusive(&d->lock);
    aedx_wait_all_locked(t);
    aedx_frames_free(t);
    int rc = aedx_frames_alloc(t, count);
    ReleaseSRWLockExclusive(&d->lock);
    return rc;
}

int aedx_target_set_timeout_ms(AedxTarget* t, int ms) {
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    if (ms <= 0) return aedx_fail(AEDX_ERR_ARG, "timeout must be positive (got %d)", ms);
    t->timeout_ms = (DWORD)ms;
    return AEDX_OK;
}

/* ------------------------------------------------------------------------ */
/* Geometry                                                                  */
/* ------------------------------------------------------------------------ */

/* Vertex and index memory is an UPLOAD heap buffer the GPU reads directly,
 * mapped for its lifetime, so geometry crosses from Aether with no staging
 * copy; a larger reservation waits for the target to be idle and replaces
 * it. */
static int aedx_grow_upload(AedxTarget* t, ID3D12Resource** res, unsigned char** ptr, int* capacity, int need) {
    if (need <= *capacity && *res) return AEDX_OK;
    AedxDevice* d = t->dev;
    AcquireSRWLockExclusive(&d->lock);
    int rc = aedx_wait_all_locked(t);
    ReleaseSRWLockExclusive(&d->lock);
    if (rc != AEDX_OK) return rc;
    if (*res) { ID3D12Resource_Release(*res); *res = NULL; *ptr = NULL; *capacity = 0; }
    ID3D12Resource* r = aedx_make_buffer(d, D3D12_HEAP_TYPE_UPLOAD, (UINT64)need,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    if (!r) return AEDX_ERR_OOM;
    D3D12_RANGE none = { 0, 0 };   /* the CPU never reads it */
    HRESULT hr = ID3D12Resource_Map(r, 0, &none, (void**)ptr);
    if (FAILED(hr)) {
        ID3D12Resource_Release(r);
        return aedx_fail(AEDX_ERR_OOM, "Map (upload) failed (0x%08lx)", (unsigned long)hr);
    }
    memset(*ptr, 0, (size_t)need);
    *res = r;
    *capacity = need;
    return AEDX_OK;
}

int aedx_ae_verts_reserve_n(void* tp, int count, int floats_per_vertex) {
    AedxTarget* t = (AedxTarget*)tp;
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    if (count <= 0) return aedx_fail(AEDX_ERR_ARG, "vertex count must be positive, got %d", count);
    if (floats_per_vertex <= 0) {
        return aedx_fail(AEDX_ERR_ARG, "floats per vertex must be positive, got %d", floats_per_vertex);
    }
    if ((long long)count * floats_per_vertex * 4 > 0x7fffffffLL) {
        return aedx_fail(AEDX_ERR_ARG, "vertex count %d is too large", count);
    }
    int rc = aedx_grow_upload(t, &t->vbuf, &t->vbuf_ptr, &t->vbuf_capacity, count * floats_per_vertex * 4);
    if (rc != AEDX_OK) return rc;
    t->vertex_count = count;
    t->vertex_floats = floats_per_vertex;
    return AEDX_OK;
}

int aedx_ae_verts_reserve(void* t, int count) { return aedx_ae_verts_reserve_n(t, count, 5); }

int aedx_ae_verts_set_float(void* tp, int index, double value) {
    AedxTarget* t = (AedxTarget*)tp;
    aedx_clear_error();
    if (!t || !t->vbuf_ptr) return aedx_fail(AEDX_ERR_ARG, "reserve vertices first");
    int total = t->vertex_count * t->vertex_floats;
    if (index < 0 || index >= total) {
        return aedx_fail(AEDX_ERR_ARG, "float index %d is outside 0..%d", index, total - 1);
    }
    float f = (float)value;
    memcpy(t->vbuf_ptr + (size_t)index * 4u, &f, 4);
    return AEDX_OK;
}

int aedx_ae_verts_set(void* tp, int index, double x, double y, double r, double g, double b) {
    AedxTarget* t = (AedxTarget*)tp;
    aedx_clear_error();
    if (!t || !t->vbuf_ptr) return aedx_fail(AEDX_ERR_ARG, "reserve vertices first");
    if (t->vertex_floats != 5) {
        return aedx_fail(AEDX_ERR_ARG, "verts_set writes the built-in 5-float layout; this target has %d",
                         t->vertex_floats);
    }
    if (index < 0 || index >= t->vertex_count) {
        return aedx_fail(AEDX_ERR_ARG, "vertex %d is outside 0..%d", index, t->vertex_count - 1);
    }
    float v[5] = { (float)x, (float)y, (float)r, (float)g, (float)b };
    memcpy(t->vbuf_ptr + (size_t)index * 20u, v, sizeof(v));
    return AEDX_OK;
}

int aedx_ae_indices_reserve_ex(void* tp, int count, int bits) {
    AedxTarget* t = (AedxTarget*)tp;
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    if (count < 0) return aedx_fail(AEDX_ERR_ARG, "index count must not be negative");
    if (bits != 16 && bits != 32) {
        return aedx_fail(AEDX_ERR_ARG, "index width must be 16 or 32 bits (got %d)", bits);
    }
    /* Checked before it is multiplied: a count whose byte size wraps an int
     * would reserve a few bytes and then accept writes far past them. */
    if ((long long)count * (bits / 8) > 0x7fffffffLL) {
        return aedx_fail(AEDX_ERR_ARG, "index count %d is too large", count);
    }
    int need = count * (bits / 8);
    if (count > 0) {
        int rc = aedx_grow_upload(t, &t->ibuf, &t->ibuf_ptr, &t->ibuf_capacity, need);
        if (rc != AEDX_OK) return rc;
    }
    t->index_count = count;
    t->index_bits = bits;
    return AEDX_OK;
}

int aedx_ae_indices_reserve(void* t, int count) { return aedx_ae_indices_reserve_ex(t, count, 32); }

int aedx_ae_indices_set(void* tp, int index, int value) {
    AedxTarget* t = (AedxTarget*)tp;
    aedx_clear_error();
    if (!t || !t->ibuf_ptr || t->index_count == 0) return aedx_fail(AEDX_ERR_ARG, "reserve indices first");
    if (index < 0 || index >= t->index_count) {
        return aedx_fail(AEDX_ERR_ARG, "index %d is outside 0..%d", index, t->index_count - 1);
    }
    if (value < 0) return aedx_fail(AEDX_ERR_ARG, "vertex index must not be negative");
    if (value >= t->vertex_count) {
        return aedx_fail(AEDX_ERR_ARG, "index %d points past the %d uploaded vertices", value, t->vertex_count);
    }
    if (t->index_bits == 16) {
        if (value > 65535) return aedx_fail(AEDX_ERR_ARG, "vertex index %d does not fit in a 16-bit index", value);
        uint16_t v = (uint16_t)value;
        memcpy(t->ibuf_ptr + (size_t)index * 2u, &v, 2);
    } else {
        uint32_t v = (uint32_t)value;
        memcpy(t->ibuf_ptr + (size_t)index * 4u, &v, 4);
    }
    return AEDX_OK;
}

int aedx_target_set_push(AedxTarget* t, const void* data, size_t len) {
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    if (len > AEDX_MAX_PUSH || (len % 4)) {
        return aedx_fail(AEDX_ERR_ARG, "push constants are at most %d bytes in whole 4-byte words, got %zu",
                         AEDX_MAX_PUSH, len);
    }
    if (len > 0 && !data) return aedx_fail(AEDX_ERR_ARG, "push data is null");
    if (len > 0) memcpy(t->push, data, len);
    t->push_size = (UINT)len;
    return AEDX_OK;
}

int aedx_ae_push_floats(void* tp, int count) {
    AedxTarget* t = (AedxTarget*)tp;
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    if (count < 0 || (size_t)count * 4u > AEDX_MAX_PUSH) {
        return aedx_fail(AEDX_ERR_ARG, "push block of %d floats exceeds %d bytes", count, AEDX_MAX_PUSH);
    }
    memset(t->push, 0, sizeof(t->push));
    t->push_size = (UINT)count * 4u;
    return AEDX_OK;
}

int aedx_ae_push_float(void* tp, int index, double value) {
    AedxTarget* t = (AedxTarget*)tp;
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    int n = (int)(t->push_size / 4u);
    if (index < 0 || index >= n) return aedx_fail(AEDX_ERR_ARG, "push float %d is outside 0..%d", index, n - 1);
    float f = (float)value;
    memcpy(t->push + (size_t)index * 4u, &f, 4);
    return AEDX_OK;
}

int aedx_batch_reset(AedxTarget* t) {
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    t->batch_count = 0;
    return AEDX_OK;
}

static int aedx_batch_push(AedxTarget* t, const AedxDrawItem* item) {
    if (t->batch_count == t->batch_cap) {
        int cap = t->batch_cap ? t->batch_cap * 2 : 8;
        AedxDrawItem* grown = (AedxDrawItem*)realloc(t->batch, (size_t)cap * sizeof(*grown));
        if (!grown) return aedx_fail(AEDX_ERR_OOM, "out of memory");
        t->batch = grown;
        t->batch_cap = cap;
    }
    t->batch[t->batch_count++] = *item;
    return AEDX_OK;
}

static int aedx_batch_add_range(AedxTarget* t, AedxMaterial* mat, int first, int count,
                                int first_instance, int instances) {
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    if (first < 0) return aedx_fail(AEDX_ERR_ARG, "first must not be negative, got %d", first);
    if (count <= 0) return aedx_fail(AEDX_ERR_ARG, "draw count must be positive, got %d", count);
    int limit = t->index_count > 0 ? t->index_count : t->vertex_count;
    if (limit > 0 && (long long)first + count > limit) {
        return aedx_fail(AEDX_ERR_ARG, "draw covers %d..%lld but only %d are uploaded",
                         first, (long long)first + count - 1, limit);
    }
    AedxDrawItem it;
    memset(&it, 0, sizeof(it));
    it.mat = mat;
    it.first = first;
    it.count = count;
    it.first_instance = first_instance;
    it.instances = instances;
    return aedx_batch_push(t, &it);
}

int aedx_batch_add(AedxTarget* t, AedxMaterial* mat, int first, int count) {
    return aedx_batch_add_range(t, mat, first, count, 0, 0);
}

/* Instancing (#2198): `instances` copies of the range from instance
 * `first_instance`, where a per-instance stream starts reading. */
int aedx_batch_add_instanced(AedxTarget* t, AedxMaterial* mat, int first, int count,
                             int first_instance, int instances) {
    if (first_instance < 0) {
        return aedx_fail(AEDX_ERR_ARG, "first instance must not be negative, got %d", first_instance);
    }
    if (instances <= 0) return aedx_fail(AEDX_ERR_ARG, "instances must be positive, got %d", instances);
    return aedx_batch_add_range(t, mat, first, count, first_instance, instances);
}

/* An indirect draw (#2198): `draws` commands read from `buf` at `offset` when
 * the frame runs, five 32-bit words each with indices and four without. */
int aedx_batch_add_indirect(AedxTarget* t, AedxMaterial* mat, AedxBuffer* buf, int offset, int draws) {
    aedx_clear_error();
    if (!t || !buf) return aedx_fail(AEDX_ERR_ARG, "target or buffer is null");
    if (buf->dev != t->dev) return aedx_fail(AEDX_ERR_ARG, "the buffer belongs to another device");
    if (offset < 0 || (offset % 4)) {
        return aedx_fail(AEDX_ERR_ARG, "an indirect offset must be a non-negative multiple of 4, got %d",
                         offset);
    }
    if (draws <= 0) return aedx_fail(AEDX_ERR_ARG, "draws must be positive, got %d", draws);
    AedxDrawItem it;
    memset(&it, 0, sizeof(it));
    it.mat = mat;
    it.indirect = buf;
    it.indirect_offset = (UINT64)offset;
    it.indirect_draws = draws;
    return aedx_batch_push(t, &it);
}

/* A root constant buffer view's address must be a multiple of 256. */
#define AEDX_UNIFORM_ALIGN D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT

int aedx_batch_set_offset(AedxTarget* t, int item, int binding, int offset) {
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    if (binding < 0 || binding >= AEDX_MAX_DESC) {
        return aedx_fail(AEDX_ERR_ARG, "binding must be 0..%d", AEDX_MAX_DESC - 1);
    }
    if (offset < 0 || (offset % AEDX_UNIFORM_ALIGN)) {
        return aedx_fail(AEDX_ERR_ARG,
                         "a uniform offset must be a non-negative multiple of %d on this device, got %d",
                         AEDX_UNIFORM_ALIGN, offset);
    }
    if (item < 0 || item >= t->batch_count) {
        return aedx_fail(AEDX_ERR_ARG, "draw %d is outside the batch of %d", item, t->batch_count);
    }
    t->batch[item].offsets[binding] = (UINT)offset;
    return AEDX_OK;
}

int aedx_uniform_offset_alignment(const AedxDevice* d) { return d ? AEDX_UNIFORM_ALIGN : 0; }

/* Draw `item`'s scissor (#2398), checked against the target when the frame
 * is drawn; a width of 0 goes back to the whole target. */
int aedx_batch_set_scissor(AedxTarget* t, int item, int x, int y, int w, int h) {
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    if (x < 0 || y < 0 || w < 0 || h < 0) {
        return aedx_fail(AEDX_ERR_ARG, "a scissor %d,%d %dx%d is not inside a target", x, y, w, h);
    }
    if (item < 0 || item >= t->batch_count) {
        return aedx_fail(AEDX_ERR_ARG, "draw %d is outside the batch of %d", item, t->batch_count);
    }
    AedxDrawItem* it = &t->batch[item];
    it->scissor_set = w > 0 && h > 0;
    it->sx = x; it->sy = y; it->sw = w; it->sh = h;
    return AEDX_OK;
}

/* Draw `item`'s stencil reference (#2399), 0..255; -1 goes back to the
 * pipeline state's. */
int aedx_batch_set_stencil_ref(AedxTarget* t, int item, int ref) {
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    if (ref < -1 || ref > 255) return aedx_fail(AEDX_ERR_ARG, "a stencil reference is 0..255 (or -1), not %d", ref);
    if (item < 0 || item >= t->batch_count) {
        return aedx_fail(AEDX_ERR_ARG, "draw %d is outside the batch of %d", item, t->batch_count);
    }
    AedxDrawItem* it = &t->batch[item];
    it->stencil_ref_set = ref >= 0;
    it->stencil_ref = ref >= 0 ? (UINT)ref : 0;
    return AEDX_OK;
}

/* Draw `item`'s viewport (#2398); a width of 0 goes back to the whole
 * target. */
int aedx_batch_set_viewport(AedxTarget* t, int item, float x, float y, float w, float h,
                           float min_depth, float max_depth) {
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    if (w < 0.0f || h < 0.0f || !(min_depth >= 0.0f && max_depth <= 1.0f && min_depth <= max_depth)) {
        return aedx_fail(AEDX_ERR_ARG, "a viewport needs a size of at least 0 and depths in 0 <= min <= max <= 1");
    }
    if (item < 0 || item >= t->batch_count) {
        return aedx_fail(AEDX_ERR_ARG, "draw %d is outside the batch of %d", item, t->batch_count);
    }
    AedxDrawItem* it = &t->batch[item];
    it->viewport_set = w > 0.0f && h > 0.0f;
    it->vx = x; it->vy = y; it->vw = w; it->vh = h; it->vmin = min_depth; it->vmax = max_depth;
    return AEDX_OK;
}


int aedx_target_set_stream(AedxTarget* t, int binding, AedxBuffer* buf) {
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    if (binding < 1 || binding >= AEDX_MAX_BINDINGS) {
        return aedx_fail(AEDX_ERR_ARG,
                         "a vertex stream binding is 1..%d; binding 0 is the target's own vertices",
                         AEDX_MAX_BINDINGS - 1);
    }
    if (buf && buf->dev != t->dev) return aedx_fail(AEDX_ERR_ARG, "the buffer belongs to another device");
    t->streams[binding] = buf;
    return AEDX_OK;
}

int aedx_target_set_instances(AedxTarget* t, int count) {
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    if (count <= 0) return aedx_fail(AEDX_ERR_ARG, "instances must be positive, got %d", count);
    t->instance_count = count;
    return AEDX_OK;
}

int aedx_target_instances(const AedxTarget* t) { return t ? t->instance_count : 0; }

int aedx_target_set_timing(AedxTarget* t, int on) {
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    on = on ? 1 : 0;
    AedxDevice* d = t->dev;
    AcquireSRWLockExclusive(&d->lock);
    int rc = AEDX_OK;
    if (on && !t->timer_heap) {
        rc = aedx_make_timer(d, 2u * AEDX_MAX_FRAMES, &t->timer_heap, &t->timer_rb, &t->timer_ptr);
    }
    if (rc == AEDX_OK) {
        t->timing_on = on;
        t->gpu_ms = -1.0;
    }
    ReleaseSRWLockExclusive(&d->lock);
    return rc;
}

double aedx_target_gpu_ms(const AedxTarget* t) { return (t && t->timing_on) ? t->gpu_ms : -1.0; }

int aedx_batch_count(const AedxTarget* t) { return t ? t->batch_count : 0; }

/* ------------------------------------------------------------------------ */
/* Vertex layouts and bindings                                               */
/* ------------------------------------------------------------------------ */

struct AedxLayout {
    D3D12_INPUT_ELEMENT_DESC el[AEDX_MAX_ATTRS];
    int  attr_count;
    int  stride[AEDX_MAX_BINDINGS];
    int  per_instance[AEDX_MAX_BINDINGS];
    int  declared[AEDX_MAX_BINDINGS];
    int  bind_count;
};

/* The HLSL input semantic every attribute uses: location N is TEXCOORDN. */
static const char k_semantic[] = "TEXCOORD";

AedxLayout* aedx_layout_create(void) {
    aedx_clear_error();
    AedxLayout* l = (AedxLayout*)calloc(1, sizeof(*l));
    if (!l) aedx_fail(AEDX_ERR_OOM, "out of memory");
    return l;
}

void aedx_layout_destroy(AedxLayout* l) { free(l); }

int aedx_layout_binding(AedxLayout* l, int binding, int stride, int per_instance) {
    aedx_clear_error();
    if (!l) return aedx_fail(AEDX_ERR_ARG, "layout is null");
    if (binding < 0 || binding >= AEDX_MAX_BINDINGS || stride <= 0) {
        return aedx_fail(AEDX_ERR_ARG, "binding %d stride %d is not a stream (bindings 0..%d)",
                         binding, stride, AEDX_MAX_BINDINGS - 1);
    }
    if (l->declared[binding]) return aedx_fail(AEDX_ERR_ARG, "binding %d is already declared", binding);
    l->declared[binding] = 1;
    l->stride[binding] = stride;
    l->per_instance[binding] = per_instance ? 1 : 0;
    l->bind_count++;
    return AEDX_OK;
}

int aedx_layout_attr(AedxLayout* l, int location, int binding, int format, int offset) {
    aedx_clear_error();
    if (!l) return aedx_fail(AEDX_ERR_ARG, "layout is null");
    if (location < 0 || binding < 0 || binding >= AEDX_MAX_BINDINGS || offset < 0) {
        return aedx_fail(AEDX_ERR_ARG, "location, binding and offset must not be negative");
    }
    if (format <= 0) return aedx_fail(AEDX_ERR_ARG, "format %d is not a DXGI_FORMAT", format);
    if (l->attr_count >= AEDX_MAX_ATTRS) return aedx_fail(AEDX_ERR_ARG, "at most %d vertex attributes", AEDX_MAX_ATTRS);
    D3D12_INPUT_ELEMENT_DESC* e = &l->el[l->attr_count++];
    memset(e, 0, sizeof(*e));
    e->SemanticName = k_semantic;
    e->SemanticIndex = (UINT)location;
    e->Format = (DXGI_FORMAT)format;
    e->InputSlot = (UINT)binding;
    e->AlignedByteOffset = (UINT)offset;
    /* The rate is filled in from the binding when the pipeline is made, so
     * attributes and bindings may be declared in either order. */
    return AEDX_OK;
}

enum { AEDX_BIND_NONE = 0, AEDX_BIND_UNIFORM, AEDX_BIND_TEXTURE, AEDX_BIND_STORAGE,
       AEDX_BIND_UNIFORM_DYNAMIC, AEDX_BIND_STORAGE_TEXTURE };

struct AedxBindings {
    int  kind[AEDX_MAX_DESC];
    int  count;
    UINT dyn_range[AEDX_MAX_DESC];   /* a dynamic uniform's window (#2198) */
};

AedxBindings* aedx_bindings_create(void) {
    aedx_clear_error();
    AedxBindings* b = (AedxBindings*)calloc(1, sizeof(*b));
    if (!b) aedx_fail(AEDX_ERR_OOM, "out of memory");
    return b;
}

void aedx_bindings_destroy(AedxBindings* b) { free(b); }

static int aedx_bindings_add(AedxBindings* b, int binding, int kind) {
    aedx_clear_error();
    if (!b) return aedx_fail(AEDX_ERR_ARG, "bindings is null");
    if (binding < 0 || binding >= AEDX_MAX_DESC) {
        return aedx_fail(AEDX_ERR_ARG, "binding must be 0..%d", AEDX_MAX_DESC - 1);
    }
    if (b->kind[binding]) return aedx_fail(AEDX_ERR_ARG, "binding %d is already declared", binding);
    b->kind[binding] = kind;
    b->count++;
    return AEDX_OK;
}

int aedx_bindings_uniform(AedxBindings* b, int binding) { return aedx_bindings_add(b, binding, AEDX_BIND_UNIFORM); }
int aedx_bindings_texture(AedxBindings* b, int binding) { return aedx_bindings_add(b, binding, AEDX_BIND_TEXTURE); }
int aedx_bindings_storage(AedxBindings* b, int binding) { return aedx_bindings_add(b, binding, AEDX_BIND_STORAGE); }

/* A texture a compute pass writes (#2388): a RWTexture2D or RWTexture3D at
 * uN, through a descriptor table. Compute passes only. */
int aedx_bindings_storage_texture(AedxBindings* b, int binding) {
    return aedx_bindings_add(b, binding, AEDX_BIND_STORAGE_TEXTURE);
}

/* A uniform read as a `bytes` window of a caller's buffer, at the offset
 * each draw chooses: a root CBV whose address moves per draw (#2198). */
int aedx_bindings_uniform_dynamic(AedxBindings* b, int binding, int bytes) {
    aedx_clear_error();
    if (bytes <= 0) return aedx_fail(AEDX_ERR_ARG, "a dynamic uniform's window must be positive, got %d", bytes);
    int rc = aedx_bindings_add(b, binding, AEDX_BIND_UNIFORM_DYNAMIC);
    if (rc == AEDX_OK) b->dyn_range[binding] = (UINT)bytes;
    return rc;
}

/* A root signature for a set of bindings and a push block, and where each
 * binding's argument lives in it. Push constants are root constants at b0
 * in space1; a uniform at binding N is a root CBV at bN, a storage buffer a
 * root UAV at uN, and a texture two descriptor tables, its SRV at tN and its
 * sampler at sN. Visible to every stage, as Vulkan's bindings are. */
typedef struct {
    ID3D12RootSignature* root;
    int                  kind[AEDX_MAX_DESC];
    int                  param[AEDX_MAX_DESC];   /* root parameter index */
    UINT                 dyn_range[AEDX_MAX_DESC];
    UINT                 push_words;
} AedxRootLayout;

static int aedx_make_root(AedxDevice* d, const AedxBindings* b, int push_bytes, int compute, AedxRootLayout* out) {
    D3D12_ROOT_PARAMETER params[1 + AEDX_MAX_DESC * 2];
    D3D12_DESCRIPTOR_RANGE ranges[AEDX_MAX_DESC * 2];
    memset(params, 0, sizeof(params));
    memset(ranges, 0, sizeof(ranges));
    UINT np = 0, nr = 0;
    memset(out, 0, sizeof(*out));
    out->push_words = (UINT)push_bytes / 4u;
    if (push_bytes > 0) {
        params[np].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[np].Constants.ShaderRegister = 0;
        params[np].Constants.RegisterSpace = 1;
        params[np].Constants.Num32BitValues = out->push_words;
        params[np].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        np++;
    }
    for (int i = 0; b && i < AEDX_MAX_DESC; i++) {
        out->kind[i] = b->kind[i];
        out->param[i] = -1;
        if (!b->kind[i]) continue;
        if (compute && b->kind[i] == AEDX_BIND_UNIFORM_DYNAMIC) {
            return aedx_fail(AEDX_ERR_ARG,
                             "binding %d is a dynamic uniform, which draws take and compute passes do not", i);
        }
        if (!compute && b->kind[i] == AEDX_BIND_STORAGE_TEXTURE) {
            return aedx_fail(AEDX_ERR_ARG,
                             "binding %d is a storage texture, which compute passes take and draws do not", i);
        }
        out->dyn_range[i] = b->dyn_range[i];
        out->param[i] = (int)np;
        switch (b->kind[i]) {
            case AEDX_BIND_UNIFORM:
            case AEDX_BIND_UNIFORM_DYNAMIC:
                params[np].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                params[np].Descriptor.ShaderRegister = (UINT)i;
                params[np].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                np++;
                break;
            case AEDX_BIND_STORAGE:
                params[np].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
                params[np].Descriptor.ShaderRegister = (UINT)i;
                params[np].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                np++;
                break;
            case AEDX_BIND_STORAGE_TEXTURE:
                ranges[nr].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                ranges[nr].NumDescriptors = 1;
                ranges[nr].BaseShaderRegister = (UINT)i;
                params[np].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[np].DescriptorTable.NumDescriptorRanges = 1;
                params[np].DescriptorTable.pDescriptorRanges = &ranges[nr++];
                params[np].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                np++;
                break;
            case AEDX_BIND_TEXTURE:
                ranges[nr].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                ranges[nr].NumDescriptors = 1;
                ranges[nr].BaseShaderRegister = (UINT)i;
                params[np].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[np].DescriptorTable.NumDescriptorRanges = 1;
                params[np].DescriptorTable.pDescriptorRanges = &ranges[nr++];
                params[np].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                np++;
                ranges[nr].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                ranges[nr].NumDescriptors = 1;
                ranges[nr].BaseShaderRegister = (UINT)i;
                params[np].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[np].DescriptorTable.NumDescriptorRanges = 1;
                params[np].DescriptorTable.pDescriptorRanges = &ranges[nr++];
                params[np].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                np++;
                break;
            default:
                break;
        }
    }
    D3D12_ROOT_SIGNATURE_DESC rs;
    memset(&rs, 0, sizeof(rs));
    rs.NumParameters = np;
    rs.pParameters = np ? params : NULL;
    rs.Flags = compute ? D3D12_ROOT_SIGNATURE_FLAG_NONE
                       : D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* blob = NULL;
    ID3DBlob* errors = NULL;
    HRESULT hr = g_dx.serialize_root(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (FAILED(hr)) {
        const char* msg = errors ? (const char*)ID3D10Blob_GetBufferPointer(errors) : "no message";
        aedx_fail(AEDX_ERR_ARG, "the root signature is invalid: %s", msg);
        if (errors) ID3D10Blob_Release(errors);
        return AEDX_ERR_ARG;
    }
    if (errors) ID3D10Blob_Release(errors);
    hr = ID3D12Device_CreateRootSignature(d->device, 0, ID3D10Blob_GetBufferPointer(blob),
                                          ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature,
                                          (void**)&out->root);
    ID3D10Blob_Release(blob);
    if (FAILED(hr)) return aedx_fail(AEDX_ERR_OOM, "CreateRootSignature failed (0x%08lx)", (unsigned long)hr);
    return AEDX_OK;
}

/* ------------------------------------------------------------------------ */
/* Textures                                                                  */
/* ------------------------------------------------------------------------ */

/* What a texture is (#2387, #2388). */
#define AEDX_TEX_2D    0
#define AEDX_TEX_3D    1
#define AEDX_TEX_CUBE  2
#define AEDX_TEX_ARRAY 3

struct AedxTexture {
    AedxDevice*     dev;
    int             width, height;
    int             depth;          /* 1 for a 2D texture; slices for a 3D one (#2198) */
    int             layers;         /* 6 for a cube, an array's layers, 1 otherwise (#2387) */
    int             kind;           /* AEDX_TEX_* */
    DXGI_FORMAT     format;
    int             bpp;            /* bytes a texel */
    int             mips;
    ID3D12Resource* res;
    int             srv_slot, sampler_slot;
    /* A storage texture (#2388): an unordered-access view in the shader-
     * visible heap. It rests in the shader-resource state like any texture,
     * and a dispatch that writes it moves it to UNORDERED_ACCESS and back. */
    int             storage;
    int             uav_slot;
    int             uploaded;
    /* A block-compressed format (#2397): `bpp` is then bytes a 4x4 block.
     * `levels_mask` has a bit for each level with pixels, and the texture
     * binds once every level has some. `aniso` is the anisotropy granted. */
    int             block;
    uint32_t        levels_mask;
    int             aniso;
};

static int aedx_mip_levels_for(int w, int h) {
    int levels = 1;
    while (w > 1 || h > 1) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        levels++;
    }
    return levels;
}

AedxTexture* aedx_texture_create(AedxDevice* d, int w, int h) {
    return aedx_texture_create_ex(d, w, h, 0, 0, 0);
}

static int aedx_format_bytes(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
        default:                             return 4;
    }
}

/* The formats a sampled texture can be in (#2397): bytes a texel, or with
 * *block set, bytes a 4x4 block. 0 for any other format. */
static int aedx_sampled_format(DXGI_FORMAT f, int* block) {
    *block = 0;
    switch (f) {
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_SNORM:            return 1;
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_SNORM:          return 2;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return 4;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:  return 8;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:  return 16;
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:           *block = 1; return 8;
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:      *block = 1; return 16;
        default:                              return 0;
    }
}

/* 1 when the device samples `format` in a 2D texture (#2397). */
static int aedx_format_samples(AedxDevice* d, DXGI_FORMAT format) {
    int block = 0;
    if (!aedx_sampled_format(format, &block)) return 0;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs;
    memset(&fs, 0, sizeof(fs));
    fs.Format = format;
    const D3D12_FORMAT_SUPPORT1 need = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
    if (FAILED(ID3D12Device_CheckFeatureSupport(d->device, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs)))) return 0;
    return (fs.Support1 & need) == need;
}

int aedx_texture_format_supported(AedxDevice* d, int format) {
    return d ? aedx_format_samples(d, (DXGI_FORMAT)format) : 0;
}

/* Bytes of one layer of `level` of a texture: texels, or 4x4 blocks
 * rounded up at the edges. */
static size_t aedx_level_bytes(const AedxTexture* tex, int level) {
    size_t w = (size_t)tex->width >> level, h = (size_t)tex->height >> level;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (tex->block) return ((w + 3) / 4) * ((h + 3) / 4) * (size_t)tex->bpp;
    return w * h * (size_t)tex->depth * (size_t)tex->bpp;
}

/* An sRGB-encoded byte as a linear value, and back (IEC 61966-2-1). */
static float aedx_srgb_to_linear(int v) {
    float c = (float)v / 255.0f;
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static unsigned char aedx_linear_to_srgb(float c) {
    float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
    int v = (int)(s * 255.0f + 0.5f);
    return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* Float to IEEE 754 half, rounding to nearest even; too large a value is an
 * infinity, and NaN stays NaN. */
static uint16_t aedx_float_to_half(float f) {
    uint32_t x;
    memcpy(&x, &f, sizeof(x));
    uint32_t sign = (x >> 16) & 0x8000u;
    uint32_t exp = (x >> 23) & 0xFFu;
    uint32_t mant = x & 0x7FFFFFu;
    if (exp == 0xFF) return (uint16_t)(sign | 0x7C00u | (mant ? 0x200u : 0u));
    int e = (int)exp - 127 + 15;
    if (e >= 0x1F) return (uint16_t)(sign | 0x7C00u);
    if (e <= 0) {
        /* A half subnormal: the significand, implicit bit included, shifted
         * down to units of 2^-24. */
        if (e < -10) return (uint16_t)sign;
        mant |= 0x800000u;
        uint32_t shift = (uint32_t)(14 - e);
        uint32_t h = mant >> shift;
        uint32_t rem = mant & ((1u << shift) - 1u), mid = 1u << (shift - 1);
        if (rem > mid || (rem == mid && (h & 1u))) h++;
        return (uint16_t)(sign | h);
    }
    uint32_t h = ((uint32_t)e << 10) | (mant >> 13);
    uint32_t rem = mant & 0x1FFFu;
    /* A carry out of the significand rounds up into the exponent, as it
     * should. */
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) h++;
    return (uint16_t)(sign | h);
}

/* The next level down of an uncompressed chain: each texel the 2x2 box
 * average of the level above (clamped at an odd edge), which is what a
 * linear 2:1 blit produces. sRGB colour averages as linear light, half and
 * float as floats, and the rest as bytes. */
static void aedx_downsample(DXGI_FORMAT f, int bpp, const unsigned char* src, int pw, int ph,
                            unsigned char* dst, int nw, int nh) {
    int chans = f == DXGI_FORMAT_R8_UNORM || f == DXGI_FORMAT_R8_SNORM ? 1
              : f == DXGI_FORMAT_R8G8_UNORM || f == DXGI_FORMAT_R8G8_SNORM ? 2 : 4;
    int snorm = f == DXGI_FORMAT_R8_SNORM || f == DXGI_FORMAT_R8G8_SNORM;
    for (int y = 0; y < nh; y++) {
        int y0 = y * 2 < ph ? y * 2 : ph - 1, y1 = y * 2 + 1 < ph ? y * 2 + 1 : ph - 1;
        for (int x = 0; x < nw; x++) {
            int x0 = x * 2 < pw ? x * 2 : pw - 1, x1 = x * 2 + 1 < pw ? x * 2 + 1 : pw - 1;
            const unsigned char* t[4] = {
                src + ((size_t)y0 * pw + x0) * bpp, src + ((size_t)y0 * pw + x1) * bpp,
                src + ((size_t)y1 * pw + x0) * bpp, src + ((size_t)y1 * pw + x1) * bpp,
            };
            unsigned char* out = dst + ((size_t)y * nw + x) * bpp;
            for (int c = 0; c < chans; c++) {
                if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    float s = 0.0f;
                    for (int k = 0; k < 4; k++) {
                        uint16_t h;
                        memcpy(&h, t[k] + c * 2, sizeof(h));
                        s += aedx_half_to_float(h);
                    }
                    uint16_t h = aedx_float_to_half(s * 0.25f);
                    memcpy(out + c * 2, &h, sizeof(h));
                } else if (f == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                    float s = 0.0f;
                    for (int k = 0; k < 4; k++) {
                        float v;
                        memcpy(&v, t[k] + c * 4, sizeof(v));
                        s += v;
                    }
                    s *= 0.25f;
                    memcpy(out + c * 4, &s, sizeof(s));
                } else if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && c < 3) {
                    float s = 0.0f;
                    for (int k = 0; k < 4; k++) s += aedx_srgb_to_linear(t[k][c]);
                    out[c] = aedx_linear_to_srgb(s * 0.25f);
                } else if (snorm) {
                    /* -128 reads as -1 like -127 does (#2402). */
                    float s = 0.0f;
                    for (int k = 0; k < 4; k++) {
                        int v = (signed char)t[k][c];
                        s += (v < -127 ? -127 : v) / 127.0f;
                    }
                    int q = (int)lrintf(s * 0.25f * 127.0f);
                    out[c] = (unsigned char)(signed char)q;
                } else {
                    int s = t[0][c] + t[1][c] + t[2][c] + t[3][c];
                    out[c] = (unsigned char)((s + 2) / 4);
                }
            }
        }
    }
}

/* A texture resource of `kind`: 2D with `layers` slices (6 for a cube), or
 * 3D with `depth`, waiting for its upload in COPY_DEST. */
static ID3D12Resource* aedx_make_texture_kind(AedxDevice* d, int w, int h, int depth, int layers, int kind,
                                              int mips, DXGI_FORMAT format, int storage) {
    D3D12_HEAP_PROPERTIES hp;
    memset(&hp, 0, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = kind == AEDX_TEX_3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT64)w;
    rd.Height = (UINT)h;
    rd.DepthOrArraySize = (UINT16)(kind == AEDX_TEX_3D ? depth : layers);
    rd.MipLevels = (UINT16)mips;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = storage ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* r = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(d->device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                      &IID_ID3D12Resource, (void**)&r);
    if (FAILED(hr)) {
        aedx_fail(aedx_hr_status(hr), "CreateCommittedResource (%dx%dx%d texture, %d layers) failed (0x%08lx)",
                  w, h, depth, layers, (unsigned long)hr);
        return NULL;
    }
    return r;
}

/* A texture of `kind` (AEDX_TEX_*), in `format`, sampled or, with
 * `storage`, also written by compute passes. */
static AedxTexture* aedx_texture_make_kind(AedxDevice* d, int w, int h, int depth, int layers, int kind,
                                           int mipmapped, int linear_filter, int repeat,
                                           DXGI_FORMAT format, int storage, int aniso) {
    aedx_clear_error();
    if (!d) { aedx_fail(AEDX_ERR_ARG, "device is null"); return NULL; }
    if (w <= 0 || h <= 0 || depth <= 0 || layers <= 0) {
        aedx_fail(AEDX_ERR_ARG, "texture size %dx%dx%d with %d layers is not positive", w, h, depth, layers);
        return NULL;
    }
    int limit = kind == AEDX_TEX_3D ? D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION
              : kind == AEDX_TEX_CUBE ? D3D12_REQ_TEXTURECUBE_DIMENSION : D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    if (w > limit || h > limit || depth > limit) {
        aedx_fail(AEDX_ERR_UNSUPPORTED, "texture %dx%dx%d exceeds the Direct3D 12 limit of %d",
                  w, h, depth, limit);
        return NULL;
    }
    if (layers > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) {
        aedx_fail(AEDX_ERR_UNSUPPORTED, "%d layers exceeds the Direct3D 12 limit of %d", layers,
                  D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION);
        return NULL;
    }
    if (storage) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs;
        memset(&fs, 0, sizeof(fs));
        fs.Format = format;
        if (FAILED(ID3D12Device_CheckFeatureSupport(d->device, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) ||
            !(fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)) {
            aedx_fail(AEDX_ERR_UNSUPPORTED, "the device cannot write format %d from a compute pass", (int)format);
            return NULL;
        }
    }
    int block = 0;
    int texel_bytes = aedx_sampled_format(format, &block);
    if (!storage) {
        if ((int)format > 1000 && (int)format < 1200) {
            aedx_fail(AEDX_ERR_UNSUPPORTED, "Direct3D 12 has no ETC2 or ASTC formats (format %d)", (int)format);
            return NULL;
        }
        if (!texel_bytes) {
            aedx_fail(AEDX_ERR_ARG, "format %d is not a texture format", (int)format);
            return NULL;
        }
        if (block && kind == AEDX_TEX_3D) {
            aedx_fail(AEDX_ERR_UNSUPPORTED, "a 3D texture cannot be block compressed");
            return NULL;
        }
        if (block && (w % 4 != 0 || h % 4 != 0)) {
            aedx_fail(AEDX_ERR_ARG, "a block-compressed texture is whole 4x4 blocks, not %dx%d", w, h);
            return NULL;
        }
        if (!aedx_format_samples(d, format)) {
            aedx_fail(AEDX_ERR_UNSUPPORTED, "the device cannot sample format %d", (int)format);
            return NULL;
        }
    }
    AedxTexture* tex = (AedxTexture*)calloc(1, sizeof(*tex));
    if (!tex) { aedx_fail(AEDX_ERR_OOM, "out of memory"); return NULL; }
    tex->dev = d;
    tex->width = w;
    tex->height = h;
    tex->depth = depth;
    tex->layers = layers;
    tex->kind = kind;
    tex->format = format;
    tex->bpp = storage ? aedx_format_bytes(format) : texel_bytes;
    tex->block = block;
    tex->storage = storage;
    tex->mips = mipmapped ? aedx_mip_levels_for(w, h) : 1;
    tex->srv_slot = -1;
    tex->sampler_slot = -1;
    tex->uav_slot = -1;
    tex->res = aedx_make_texture_kind(d, w, h, depth, layers, kind, tex->mips, format, storage);
    if (!tex->res) goto fail;
    AcquireSRWLockExclusive(&d->lock);
    tex->srv_slot = aedx_slot_take(&d->srv, "shader-visible");
    if (tex->srv_slot >= 0) tex->sampler_slot = aedx_slot_take(&d->samplers, "sampler");
    if (storage && tex->sampler_slot >= 0) tex->uav_slot = aedx_slot_take(&d->srv, "shader-visible");
    ReleaseSRWLockExclusive(&d->lock);
    if (tex->srv_slot < 0 || tex->sampler_slot < 0 || (storage && tex->uav_slot < 0)) goto fail;
    /* A cube and an array need their view spelled out: the default view of a
     * six-slice resource is an array, not a cube. */
    D3D12_SHADER_RESOURCE_VIEW_DESC sv;
    memset(&sv, 0, sizeof(sv));
    sv.Format = format;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (kind == AEDX_TEX_CUBE) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = (UINT)tex->mips;
    } else if (kind == AEDX_TEX_ARRAY) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sv.Texture2DArray.MipLevels = (UINT)tex->mips;
        sv.Texture2DArray.ArraySize = (UINT)layers;
    } else if (kind == AEDX_TEX_3D) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = (UINT)tex->mips;
    } else {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = (UINT)tex->mips;
    }
    ID3D12Device_CreateShaderResourceView(d->device, tex->res, &sv, aedx_slot_cpu(&d->srv, tex->srv_slot));
    if (storage) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv;
        memset(&uv, 0, sizeof(uv));
        uv.Format = format;
        if (kind == AEDX_TEX_3D) {
            uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            uv.Texture3D.WSize = (UINT)depth;
        } else {
            uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        }
        ID3D12Device_CreateUnorderedAccessView(d->device, tex->res, NULL, &uv,
                                               aedx_slot_cpu(&d->srv, tex->uav_slot));
    }
    D3D12_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.Filter = linear_filter ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
    D3D12_TEXTURE_ADDRESS_MODE mode = repeat ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sd.AddressU = sd.AddressV = sd.AddressW = mode;
    sd.MaxLOD = D3D12_FLOAT32_MAX;
    sd.MaxAnisotropy = 1;
    /* Anisotropy as asked, up to the 16 every Direct3D 12 device filters
     * (#2397). The anisotropic filter is linear in all three. */
    tex->aniso = 1;
    if (aniso > 1) {
        tex->aniso = aniso < D3D12_REQ_MAXANISOTROPY ? aniso : D3D12_REQ_MAXANISOTROPY;
        sd.Filter = D3D12_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = (UINT)tex->aniso;
    }
    sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    ID3D12Device_CreateSampler(d->device, &sd, aedx_slot_cpu(&d->samplers, tex->sampler_slot));
    if (storage) {
        /* Zeroed by an upload of zeros, whatever the format, so a draw that
         * samples it before any pass has written reads zeros. */
        size_t bytes = (size_t)w * (size_t)h * (size_t)depth * (size_t)tex->bpp;
        void* zeros = calloc(1, bytes);
        if (!zeros) { aedx_fail(AEDX_ERR_OOM, "out of memory"); goto fail; }
        int urc = aedx_texture_upload(tex, zeros, bytes);
        free(zeros);
        if (urc != AEDX_OK) goto fail;
    }
    return tex;
fail:
    aedx_texture_destroy(tex);
    return NULL;
}

static AedxTexture* aedx_texture_make(AedxDevice* d, int w, int h, int depth, int is_3d,
                                      int mipmapped, int linear_filter, int repeat) {
    return aedx_texture_make_kind(d, w, h, depth, 1, is_3d ? AEDX_TEX_3D : AEDX_TEX_2D, mipmapped,
                                  linear_filter, repeat, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
}

AedxTexture* aedx_texture_create_ex(AedxDevice* d, int w, int h, int mipmapped, int linear_filter, int repeat) {
    return aedx_texture_make(d, w, h, 1, 0, mipmapped, linear_filter, repeat);
}

/* A cube map (#2387): six square faces uploaded +X, -X, +Y, -Y, +Z, -Z,
 * read through a TextureCube. */
AedxTexture* aedx_texture_create_cube(AedxDevice* d, int size, int mipmapped, int linear_filter) {
    return aedx_texture_make_kind(d, size, size, 1, 6, AEDX_TEX_CUBE, mipmapped, linear_filter, 0,
                                  DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
}

/* A cube map or a 2D array in any sampled format (#2402). */
AedxTexture* aedx_texture_create_cube_format(AedxDevice* d, int size, int format, int mipmapped,
                                             int linear_filter) {
    return aedx_texture_make_kind(d, size, size, 1, 6, AEDX_TEX_CUBE, mipmapped, linear_filter, 0,
                                  (DXGI_FORMAT)format, 0, 1);
}

AedxTexture* aedx_texture_create_array_format(AedxDevice* d, int w, int h, int layers, int format,
                                              int mipmapped, int linear_filter, int repeat) {
    return aedx_texture_make_kind(d, w, h, 1, layers, AEDX_TEX_ARRAY, mipmapped, linear_filter, repeat,
                                  (DXGI_FORMAT)format, 0, 1);
}

/* A 2D array (#2387): `layers` images uploaded one after another, read
 * through a Texture2DArray with the layer as the third coordinate. */
AedxTexture* aedx_texture_create_array(AedxDevice* d, int w, int h, int layers, int mipmapped,
                                       int linear_filter, int repeat) {
    return aedx_texture_make_kind(d, w, h, 1, layers, AEDX_TEX_ARRAY, mipmapped, linear_filter, repeat,
                                  DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
}

/* A texture compute passes write (#2388): 2D, or 3D with `depth` above 1,
 * in R8G8B8A8_UNORM, R16G16B16A16_FLOAT or R32G32B32A32_FLOAT. */
AedxTexture* aedx_texture_create_storage(AedxDevice* d, int w, int h, int depth, int format) {
    if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
        format != DXGI_FORMAT_R32G32B32A32_FLOAT) {
        aedx_fail(AEDX_ERR_ARG, "a storage texture is FORMAT_R8G8B8A8_UNORM, FORMAT_R16G16B16A16_SFLOAT "
                  "or FORMAT_R32G32B32A32_SFLOAT, not %d", format);
        return NULL;
    }
    return aedx_texture_make_kind(d, w, h, depth, 1, depth > 1 ? AEDX_TEX_3D : AEDX_TEX_2D, 0, 0, 0,
                                  (DXGI_FORMAT)format, 1, 1);
}

int aedx_texture_layers(const AedxTexture* tex) { return tex ? tex->layers : 0; }

/* A 2D texture in any sampled format (#2397), with anisotropic filtering of
 * up to `anisotropy` samples (1 for none). An uncompressed chain is built on
 * upload; a block format's is the caller's, level by level with
 * texture_upload_level. */
AedxTexture* aedx_texture_create_format(AedxDevice* d, int w, int h, int format, int mipmapped,
                                        int linear_filter, int repeat, int anisotropy) {
    if (anisotropy < 1) {
        aedx_clear_error();
        aedx_fail(AEDX_ERR_ARG, "anisotropy is at least 1, not %d", anisotropy);
        return NULL;
    }
    return aedx_texture_make_kind(d, w, h, 1, 1, AEDX_TEX_2D, mipmapped, linear_filter, repeat,
                                  (DXGI_FORMAT)format, 0, anisotropy);
}

int aedx_texture_anisotropy(const AedxTexture* tex) { return tex ? tex->aniso : 0; }

/* Whether a texture can be bound: every level has pixels. */
static int aedx_texture_ready(const AedxTexture* tex) {
    if (tex->uploaded) return AEDX_OK;
    return aedx_fail(AEDX_ERR_ARG, tex->levels_mask ? "texture has levels without pixels yet: upload each "
                                                      "level before binding"
                                                    : "texture has no pixels yet, upload before binding");
}

/* Runs a recorded copy and waits for it. A texture being re-uploaded may be
 * in use by a frame in flight, so the queue goes idle first. */
static int aedx_run_copy(AedxDevice* d, ID3D12GraphicsCommandList* list) {
    ID3D12GraphicsCommandList_Close(list);
    AcquireSRWLockExclusive(&d->lock);
    int rc = aedx_idle(d);
    UINT64 v = rc == AEDX_OK ? aedx_execute(d, list) : 0;
    if (rc == AEDX_OK && v == 0) rc = AEDX_ERR_DEVICE_LOST;
    if (rc == AEDX_OK) {
        HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
        rc = ev ? aedx_wait_value(d, v, ev, 30000) : aedx_fail(AEDX_ERR_OOM, "CreateEvent failed");
        if (ev) CloseHandle(ev);
    }
    ReleaseSRWLockExclusive(&d->lock);
    return rc;
}

/* A 3D texture: `depth` slices of w x h RGBA, read through a Texture3D. No
 * mip chain; upload is w * h * depth * 4 bytes, slice by slice. */
AedxTexture* aedx_texture_create_3d(AedxDevice* d, int w, int h, int depth, int linear_filter, int repeat) {
    return aedx_texture_make(d, w, h, depth, 1, 0, linear_filter, repeat);
}

int aedx_texture_depth(const AedxTexture* tex) { return tex ? tex->depth : 0; }

int aedx_texture_mip_levels(const AedxTexture* tex) { return tex ? tex->mips : 0; }

void aedx_texture_destroy(AedxTexture* tex) {
    if (!tex) return;
    AedxDevice* d = tex->dev;
    AcquireSRWLockExclusive(&d->lock);
    /* A draw may still be sampling it. */
    aedx_idle(d);
    aedx_slot_give(&d->srv, tex->srv_slot);
    aedx_slot_give(&d->srv, tex->uav_slot);
    aedx_slot_give(&d->samplers, tex->sampler_slot);
    ReleaseSRWLockExclusive(&d->lock);
    if (tex->res) ID3D12Resource_Release(tex->res);
    free(tex);
}

int aedx_texture_upload_level(AedxTexture* tex, int level, const void* data, size_t len);

/* Uploads level 0 and, for a mipmapped texture, the rest of the chain, each
 * level built by aedx_downsample: what a linear 2:1 blit produces, computed
 * on the CPU because Direct3D 12 has no blit to do it with. */
int aedx_texture_upload(AedxTexture* tex, const void* rgba, size_t len) {
    aedx_clear_error();
    if (!tex || !rgba) return aedx_fail(AEDX_ERR_ARG, "texture or pixel data is null");
    /* A block format's levels are each the caller's (#2397). */
    if (tex->block) return aedx_texture_upload_level(tex, 0, rgba, len);
    int bpp = tex->bpp;
    size_t layer_bytes = (size_t)tex->width * (size_t)tex->height * (size_t)tex->depth * (size_t)bpp;
    size_t need = layer_bytes * (size_t)tex->layers;
    if (len < need) {
        return aedx_fail(AEDX_ERR_ARG, "need %zu bytes for %dx%dx%d with %d layers, got %zu",
                         need, tex->width, tex->height, tex->depth, tex->layers, len);
    }
    AedxDevice* d = tex->dev;
    D3D12_RESOURCE_DESC desc = ID3D12Resource_GetDesc(tex->res);
    /* One subresource a level a layer, layer-major: level L of layer A is
     * subresource L + A * mips. */
    UINT subs = (UINT)tex->mips * (UINT)tex->layers;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp =
        (D3D12_PLACED_SUBRESOURCE_FOOTPRINT*)calloc(subs, sizeof(*fp));
    UINT* rows = (UINT*)calloc(subs, sizeof(*rows));
    UINT64* row_bytes = (UINT64*)calloc(subs, sizeof(*row_bytes));
    if (!fp || !rows || !row_bytes) {
        free(fp); free(rows); free(row_bytes);
        return aedx_fail(AEDX_ERR_OOM, "out of memory");
    }
    UINT64 total = 0;
    ID3D12Device_GetCopyableFootprints(d->device, &desc, 0, subs, 0, fp, rows, row_bytes, &total);

    ID3D12Resource* staging = aedx_make_buffer(d, D3D12_HEAP_TYPE_UPLOAD, total,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    if (!staging) { free(fp); free(rows); free(row_bytes); return AEDX_ERR_OOM; }
    unsigned char* map = NULL;
    D3D12_RANGE none = { 0, 0 };
    HRESULT hr = ID3D12Resource_Map(staging, 0, &none, (void**)&map);
    if (FAILED(hr)) {
        ID3D12Resource_Release(staging);
        free(fp); free(rows); free(row_bytes);
        return aedx_fail(AEDX_ERR_OOM, "Map (upload) failed (0x%08lx)", (unsigned long)hr);
    }

    /* Each layer in turn: its level 0 is the caller's pixels, and each later
     * level is built from the one before it, kept in a scratch buffer. */
    for (int layer = 0; layer < tex->layers; layer++) {
        unsigned char* prev = (unsigned char*)malloc(layer_bytes);
        if (!prev) {
            ID3D12Resource_Release(staging);
            free(fp); free(rows); free(row_bytes);
            return aedx_fail(AEDX_ERR_OOM, "out of memory");
        }
        memcpy(prev, (const unsigned char*)rgba + (size_t)layer * layer_bytes, layer_bytes);
        int pw = tex->width, ph = tex->height;
        for (int level = 0; level < tex->mips; level++) {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT* lf = &fp[level + layer * tex->mips];
            UINT lrows = rows[level + layer * tex->mips];
            if (level > 0) {
                int nw = pw > 1 ? pw / 2 : 1, nh = ph > 1 ? ph / 2 : 1;
                unsigned char* next = (unsigned char*)malloc((size_t)nw * (size_t)nh * (size_t)bpp);
                if (!next) {
                    free(prev);
                    ID3D12Resource_Release(staging);
                    free(fp); free(rows); free(row_bytes);
                    return aedx_fail(AEDX_ERR_OOM, "out of memory");
                }
                aedx_downsample(tex->format, bpp, prev, pw, ph, next, nw, nh);
                free(prev);
                prev = next;
                pw = nw;
                ph = nh;
            }
            /* A 3D texture's slices follow one another, each `rows` rows of
             * RowPitch bytes (a 2D texture is one slice). */
            size_t slice_pitch = (size_t)lf->Footprint.RowPitch * (size_t)lrows;
            int slices = level == 0 ? tex->depth : 1;
            for (int z = 0; z < slices; z++) {
                for (int y = 0; y < ph; y++) {
                    memcpy(map + lf->Offset + (size_t)z * slice_pitch + (size_t)y * lf->Footprint.RowPitch,
                           prev + ((size_t)z * (size_t)ph + (size_t)y) * (size_t)pw * (size_t)bpp,
                           (size_t)pw * (size_t)bpp);
                }
            }
        }
        free(prev);
    }
    ID3D12Resource_Unmap(staging, 0, NULL);

    ID3D12CommandAllocator* alloc = NULL;
    ID3D12GraphicsCommandList* list = NULL;
    int rc = aedx_make_list(d, &alloc, &list);
    if (rc == AEDX_OK) {
        ID3D12CommandAllocator_Reset(alloc);
        ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
        /* Levels with pixels rest in the shader-resource state; the rest are
         * still in COPY_DEST, where the texture was made. */
        for (UINT sub = 0; sub < subs; sub++) {
            if (tex->levels_mask & (1u << (sub % (UINT)tex->mips))) {
                aedx_barrier_sub(list, tex->res, sub, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_COPY_DEST);
            }
        }
        for (UINT sub = 0; sub < subs; sub++) {
            D3D12_TEXTURE_COPY_LOCATION dst, src;
            memset(&dst, 0, sizeof(dst));
            memset(&src, 0, sizeof(src));
            dst.pResource = tex->res;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = sub;
            src.pResource = staging;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = fp[sub];
            ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
        }
        aedx_barrier(list, tex->res, D3D12_RESOURCE_STATE_COPY_DEST,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        rc = aedx_run_copy(d, list);
    }
    if (list) ID3D12GraphicsCommandList_Release(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    ID3D12Resource_Release(staging);
    free(fp);
    free(rows);
    free(row_bytes);
    if (rc == AEDX_OK) {
        tex->uploaded = 1;
        tex->levels_mask = tex->mips >= 32 ? ~0u : (1u << tex->mips) - 1u;
    }
    return rc;
}

/* One level of every layer, as given: a block format's chain, or a chain
 * made off the GPU (#2397). Only that level's subresources move, so levels
 * already uploaded keep their pixels; the texture can be bound once every
 * level has some. */
int aedx_texture_upload_level(AedxTexture* tex, int level, const void* data, size_t len) {
    aedx_clear_error();
    if (!tex || !data) return aedx_fail(AEDX_ERR_ARG, "texture or pixel data is null");
    if (level < 0 || level >= tex->mips) {
        return aedx_fail(AEDX_ERR_ARG, "the texture has levels 0..%d, not %d", tex->mips - 1, level);
    }
    if (tex->storage) return aedx_fail(AEDX_ERR_ARG, "a storage texture is uploaded whole");
    size_t layer_bytes = aedx_level_bytes(tex, level);
    size_t need = layer_bytes * (size_t)tex->layers;
    if (len < need) return aedx_fail(AEDX_ERR_ARG, "level %d needs %zu bytes, got %zu", level, need, len);
    AedxDevice* d = tex->dev;
    D3D12_RESOURCE_DESC desc = ID3D12Resource_GetDesc(tex->res);
    UINT layers = (UINT)tex->layers;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp =
        (D3D12_PLACED_SUBRESOURCE_FOOTPRINT*)calloc(layers, sizeof(*fp));
    UINT* rows = (UINT*)calloc(layers, sizeof(*rows));
    UINT64* row_bytes = (UINT64*)calloc(layers, sizeof(*row_bytes));
    if (!fp || !rows || !row_bytes) {
        free(fp); free(rows); free(row_bytes);
        return aedx_fail(AEDX_ERR_OOM, "out of memory");
    }
    /* The level's subresource in each layer, packed one after another in the
     * staging buffer at the placement alignment. A block format's rows are
     * rows of blocks. */
    UINT64 total = 0;
    for (UINT a = 0; a < layers; a++) {
        UINT64 bytes = 0;
        ID3D12Device_GetCopyableFootprints(d->device, &desc, (UINT)level + a * (UINT)tex->mips, 1, 0,
                                           &fp[a], &rows[a], &row_bytes[a], &bytes);
        fp[a].Offset = total;
        total = (total + bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                ~(UINT64)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    }
    ID3D12Resource* staging = aedx_make_buffer(d, D3D12_HEAP_TYPE_UPLOAD, total,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    if (!staging) { free(fp); free(rows); free(row_bytes); return AEDX_ERR_OOM; }
    unsigned char* map = NULL;
    D3D12_RANGE none = { 0, 0 };
    HRESULT hr = ID3D12Resource_Map(staging, 0, &none, (void**)&map);
    if (FAILED(hr)) {
        ID3D12Resource_Release(staging);
        free(fp); free(rows); free(row_bytes);
        return aedx_fail(AEDX_ERR_OOM, "Map (upload) failed (0x%08lx)", (unsigned long)hr);
    }
    for (UINT a = 0; a < layers; a++) {
        const unsigned char* src = (const unsigned char*)data + (size_t)a * layer_bytes;
        size_t slice_pitch = (size_t)fp[a].Footprint.RowPitch * (size_t)rows[a];
        for (UINT z = 0; z < fp[a].Footprint.Depth; z++) {
            for (UINT y = 0; y < rows[a]; y++) {
                memcpy(map + fp[a].Offset + (size_t)z * slice_pitch + (size_t)y * fp[a].Footprint.RowPitch,
                       src + ((size_t)z * rows[a] + y) * (size_t)row_bytes[a], (size_t)row_bytes[a]);
            }
        }
    }
    ID3D12Resource_Unmap(staging, 0, NULL);

    uint32_t bit = 1u << (uint32_t)level;
    ID3D12CommandAllocator* alloc = NULL;
    ID3D12GraphicsCommandList* list = NULL;
    int rc = aedx_make_list(d, &alloc, &list);
    if (rc == AEDX_OK) {
        ID3D12CommandAllocator_Reset(alloc);
        ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
        const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        for (UINT a = 0; a < layers; a++) {
            UINT sub = (UINT)level + a * (UINT)tex->mips;
            if (tex->levels_mask & bit) aedx_barrier_sub(list, tex->res, sub, read, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst, src;
            memset(&dst, 0, sizeof(dst));
            memset(&src, 0, sizeof(src));
            dst.pResource = tex->res;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = sub;
            src.pResource = staging;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = fp[a];
            ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
            aedx_barrier_sub(list, tex->res, sub, D3D12_RESOURCE_STATE_COPY_DEST, read);
        }
        rc = aedx_run_copy(d, list);
    }
    if (list) ID3D12GraphicsCommandList_Release(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    ID3D12Resource_Release(staging);
    free(fp);
    free(rows);
    free(row_bytes);
    if (rc == AEDX_OK) {
        tex->levels_mask |= bit;
        uint32_t all = tex->mips >= 32 ? ~0u : (1u << tex->mips) - 1u;
        tex->uploaded = tex->levels_mask == all;
    }
    return rc;
}

/* ------------------------------------------------------------------------ */
/* Buffers                                                                   */
/* ------------------------------------------------------------------------ */

/* A buffer shaders read and write, in a custom heap the CPU sees with write-
 * back caching: the Direct3D 12 counterpart of Vulkan's host-visible
 * coherent memory. Mapped for its lifetime and zeroed; usable as a storage
 * or uniform binding and as vertices. Buffers promote implicitly from COMMON
 * to whatever state a command needs, so none is tracked. */
AedxBuffer* aedx_buffer_create(AedxDevice* d, size_t bytes) {
    aedx_clear_error();
    if (!d) { aedx_fail(AEDX_ERR_ARG, "device is null"); return NULL; }
    if (bytes == 0) { aedx_fail(AEDX_ERR_ARG, "a buffer needs at least one byte"); return NULL; }
    AedxBuffer* b = (AedxBuffer*)calloc(1, sizeof(*b));
    if (!b) { aedx_fail(AEDX_ERR_OOM, "out of memory"); return NULL; }
    b->dev = d;
    b->size = bytes;
    /* A uniform view's size must be a multiple of 256; round the resource
     * up so the same buffer can be bound either way. */
    UINT64 alloc_bytes = ((UINT64)bytes + 255u) & ~(UINT64)255u;
    D3D12_HEAP_PROPERTIES hp;
    memset(&hp, 0, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_CUSTOM;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    D3D12_RESOURCE_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = alloc_bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT hr = ID3D12Device_CreateCommittedResource(d->device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_COMMON, NULL,
                                                      &IID_ID3D12Resource, (void**)&b->res);
    if (FAILED(hr)) {
        aedx_fail(aedx_hr_status(hr), "CreateCommittedResource (buffer of %zu bytes) failed (0x%08lx)",
                  bytes, (unsigned long)hr);
        free(b);
        return NULL;
    }
    hr = ID3D12Resource_Map(b->res, 0, NULL, (void**)&b->ptr);
    if (FAILED(hr)) {
        aedx_fail(AEDX_ERR_OOM, "Map (buffer) failed (0x%08lx)", (unsigned long)hr);
        ID3D12Resource_Release(b->res);
        free(b);
        return NULL;
    }
    memset(b->ptr, 0, (size_t)alloc_bytes);
    return b;
}

void aedx_buffer_destroy(AedxBuffer* b) {
    if (!b) return;
    AedxDevice* d = b->dev;
    AcquireSRWLockExclusive(&d->lock);
    aedx_idle(d);
    ReleaseSRWLockExclusive(&d->lock);
    if (b->res) {
        ID3D12Resource_Unmap(b->res, 0, NULL);
        ID3D12Resource_Release(b->res);
    }
    free(b);
}

size_t aedx_buffer_size(const AedxBuffer* b) { return b ? b->size : 0; }

int aedx_buffer_write(AedxBuffer* b, size_t offset, const void* data, size_t len) {
    aedx_clear_error();
    if (!b || !data) return aedx_fail(AEDX_ERR_ARG, "buffer or data is null");
    if (offset > b->size || len > b->size - offset) {
        return aedx_fail(AEDX_ERR_ARG, "writing %zu bytes at %zu overruns a %zu-byte buffer", len, offset, b->size);
    }
    memcpy(b->ptr + offset, data, len);
    return AEDX_OK;
}

int aedx_buffer_read(AedxBuffer* b, size_t offset, void* out, size_t len) {
    aedx_clear_error();
    if (!b || !out) return aedx_fail(AEDX_ERR_ARG, "buffer or destination is null");
    if (offset > b->size || len > b->size - offset) {
        return aedx_fail(AEDX_ERR_ARG, "reading %zu bytes at %zu overruns a %zu-byte buffer", len, offset, b->size);
    }
    memcpy(out, b->ptr + offset, len);
    return AEDX_OK;
}

/* ------------------------------------------------------------------------ */
/* Pipelines and materials                                                   */
/* ------------------------------------------------------------------------ */

struct AedxMaterial {
    AedxPipeline* pipe;
    int           set[AEDX_MAX_DESC];
    /* Uniforms the material owns: an UPLOAD heap buffer each, mapped. */
    struct {
        ID3D12Resource* res;
        unsigned char*  ptr;
        size_t          size;
    } ub[AEDX_MAX_DESC];
    AedxBuffer*   buf[AEDX_MAX_DESC];   /* a caller's buffer at this binding */
    AedxTexture*  tex[AEDX_MAX_DESC];
    /* A target sampled at a texture binding (#2198): its colour, or its
     * depth when tgt_depth is set. Resolved to the target's current views
     * when a frame is recorded, so a resize needs nothing here. */
    AedxTarget*   tgt[AEDX_MAX_DESC];
    int           tgt_depth[AEDX_MAX_DESC];
    int           tgt_cmp[AEDX_MAX_DESC];   /* compare op, 0 for the raw depth (#2373) */
    int           tgt_att[AEDX_MAX_DESC];   /* the colour attachment a colour binding reads (#2386) */
    int           tgt_view[AEDX_MAX_DESC];  /* a layered target: 1 as an array, 2 as a cube (#2399) */
};

struct AedxPipeline {
    AedxDevice*          dev;
    AedxRootLayout       rl;
    ID3D12PipelineState* pso;
    int                  push_bytes;
    int                  vertex_input;
    UINT                 stride;       /* bytes a vertex in slot 0 */
    D3D_PRIMITIVE_TOPOLOGY prim;       /* what the vertices assemble into (#2398) */
    /* The streams the layout declares past slot 0 (#2198). */
    int                  stream_declared[AEDX_MAX_BINDINGS];
    UINT                 stream_stride[AEDX_MAX_BINDINGS];
    int                  stream_instanced[AEDX_MAX_BINDINGS];
    AedxMaterial*        def;
    UINT                 stencil_ref;  /* the state's, unless a draw has its own (#2399) */
    AedxTarget*          target;       /* the target it was made for (#2411) */
};

AedxPipeline* aedx_pipeline_create(AedxDevice* d, AedxTarget* t, const void* vs, size_t vs_len,
                                   const void* ps, size_t ps_len) {
    return aedx_pipeline_create_ex(d, t, vs, vs_len, ps, ps_len, NULL, 0, NULL);
}

AedxPipeline* aedx_pipeline_create_ex(AedxDevice* d, AedxTarget* t, const void* vs, size_t vs_len,
                                      const void* ps, size_t ps_len, const AedxLayout* layout,
                                      int push_bytes, const AedxBindings* bindings) {
    return aedx_pipeline_create_state(d, t, vs, vs_len, ps, ps_len, layout, push_bytes, bindings, NULL);
}


AedxPipeline* aedx_pipeline_create_state(AedxDevice* d, AedxTarget* t, const void* vs, size_t vs_len,
                                         const void* ps, size_t ps_len, const AedxLayout* layout,
                                         int push_bytes, const AedxBindings* bindings,
                                         const AedxState* state) {
    aedx_clear_error();
    if (!d || !t) { aedx_fail(AEDX_ERR_ARG, "device or target is null"); return NULL; }
    AedxState none;
    memset(&none, 0, sizeof(none));
    const AedxState* st = state ? state : &none;
    if (st->depth_set && !t->has_depth) {
        aedx_fail(AEDX_ERR_ARG, "the state sets a depth test, but the target was created without depth");
        return NULL;
    }
    if (st->stencil_set && !t->has_stencil) {
        aedx_fail(AEDX_ERR_ARG, "the state sets a stencil test, but the target was created without a stencil "
                  "(DEPTH_STENCIL)");
        return NULL;
    }
    if (st->blend) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT bs;
        memset(&bs, 0, sizeof(bs));
        for (int c = 0; c <= t->extra; c++) {
            bs.Format = c == 0 ? t->format : t->xformat[c - 1];
            if (FAILED(ID3D12Device_CheckFeatureSupport(d->device, D3D12_FEATURE_FORMAT_SUPPORT, &bs,
                                                        sizeof(bs))) ||
                !(bs.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE)) {
                aedx_fail(AEDX_ERR_UNSUPPORTED, "the device cannot blend attachment %d's format %d", c,
                          (int)bs.Format);
                return NULL;
            }
        }
    }
    if (push_bytes < 0 || push_bytes > AEDX_MAX_PUSH || (push_bytes % 4)) {
        aedx_fail(AEDX_ERR_ARG, "push constant block must be 0..%d bytes and a multiple of 4 (got %d)",
                  AEDX_MAX_PUSH, push_bytes);
        return NULL;
    }
    ID3DBlob* vblob = NULL;
    ID3DBlob* pblob = NULL;
    if (aedx_shader(vs, vs_len, "vs_5_1", &vblob) != AEDX_OK) return NULL;
    if (aedx_shader(ps, ps_len, "ps_5_1", &pblob) != AEDX_OK) { ID3D10Blob_Release(vblob); return NULL; }

    AedxPipeline* p = (AedxPipeline*)calloc(1, sizeof(*p));
    if (!p) { aedx_fail(AEDX_ERR_OOM, "out of memory"); goto fail; }
    p->dev = d;
    p->target = t;
    p->push_bytes = push_bytes;
    if (aedx_make_root(d, bindings, push_bytes, 0, &p->rl) != AEDX_OK) goto fail;

    /* The built-in layout: float2 position at TEXCOORD0 and float3 colour at
     * TEXCOORD1, 20 bytes a vertex. An empty caller layout means no input. */
    D3D12_INPUT_ELEMENT_DESC builtin[2];
    memset(builtin, 0, sizeof(builtin));
    builtin[0].SemanticName = k_semantic;
    builtin[0].SemanticIndex = 0;
    builtin[0].Format = DXGI_FORMAT_R32G32_FLOAT;
    builtin[0].AlignedByteOffset = 0;
    builtin[1].SemanticName = k_semantic;
    builtin[1].SemanticIndex = 1;
    builtin[1].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    builtin[1].AlignedByteOffset = 8;
    D3D12_INPUT_ELEMENT_DESC custom[AEDX_MAX_ATTRS];
    D3D12_INPUT_LAYOUT_DESC il;
    memset(&il, 0, sizeof(il));
    if (!layout) {
        il.pInputElementDescs = builtin;
        il.NumElements = 2;
        p->stride = 20;
        p->vertex_input = 1;
    } else {
        /* Slot 0 is the target's own vertices; each slot past it is a
         * stream the target is given a buffer for (vertex_stream), checked
         * at draw time (#2198). */
        for (int b = 1; b < AEDX_MAX_BINDINGS; b++) {
            if (!layout->declared[b]) continue;
            p->stream_declared[b] = 1;
            p->stream_stride[b] = (UINT)layout->stride[b];
            p->stream_instanced[b] = layout->per_instance[b];
        }
        for (int i = 0; i < layout->attr_count; i++) {
            custom[i] = layout->el[i];
            UINT slot = custom[i].InputSlot;
            if (!layout->declared[slot]) {
                aedx_fail(AEDX_ERR_ARG, "attribute %d reads binding %u, which was never declared", i, slot);
                goto fail;
            }
            if (layout->per_instance[slot]) {
                custom[i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
                custom[i].InstanceDataStepRate = 1;
            }
        }
        il.pInputElementDescs = layout->attr_count ? custom : NULL;
        il.NumElements = (UINT)layout->attr_count;
        p->stride = (UINT)layout->stride[0];
        p->vertex_input = layout->bind_count > 0;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd;
    memset(&gd, 0, sizeof(gd));
    gd.pRootSignature = p->rl.root;
    gd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vblob);
    gd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vblob);
    gd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(pblob);
    gd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(pblob);
    D3D12_RENDER_TARGET_BLEND_DESC* rtb = &gd.BlendState.RenderTarget[0];
    /* COLOR_* is D3D12_COLOR_WRITE_ENABLE's numbering (#2411). */
    rtb->RenderTargetWriteMask = st->color_mask_set ? (UINT8)st->color_mask : (UINT8)D3D12_COLOR_WRITE_ENABLE_ALL;
    rtb->BlendEnable = st->blend ? TRUE : FALSE;
    rtb->BlendOp = D3D12_BLEND_OP_ADD;
    rtb->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rtb->LogicOp = D3D12_LOGIC_OP_NOOP;
    switch (st->blend) {
        case 1:   /* alpha */
            rtb->SrcBlend = D3D12_BLEND_SRC_ALPHA;
            rtb->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            rtb->SrcBlendAlpha = D3D12_BLEND_ONE;
            rtb->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case 2:   /* premultiplied */
            rtb->SrcBlend = D3D12_BLEND_ONE;
            rtb->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            rtb->SrcBlendAlpha = D3D12_BLEND_ONE;
            rtb->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case 3:   /* additive */
            rtb->SrcBlend = D3D12_BLEND_ONE;
            rtb->DestBlend = D3D12_BLEND_ONE;
            rtb->SrcBlendAlpha = D3D12_BLEND_ONE;
            rtb->DestBlendAlpha = D3D12_BLEND_ONE;
            break;
        default:
            rtb->SrcBlend = D3D12_BLEND_ONE;
            rtb->DestBlend = D3D12_BLEND_ZERO;
            rtb->SrcBlendAlpha = D3D12_BLEND_ONE;
            rtb->DestBlendAlpha = D3D12_BLEND_ZERO;
            break;
    }
    gd.SampleMask = UINT_MAX;
    gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gd.RasterizerState.CullMode = st->cull == 1 ? D3D12_CULL_MODE_BACK
                                : st->cull == 2 ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_NONE;
    /* The render target's y points down like the screen's, so its
     * counter-clockwise is the screen's. */
    gd.RasterizerState.FrontCounterClockwise = TRUE;
    gd.RasterizerState.DepthClipEnable = TRUE;
    gd.RasterizerState.MultisampleEnable = t->samples > 1;
    gd.DepthStencilState.DepthEnable = t->has_depth ? TRUE : FALSE;
    gd.DepthStencilState.DepthWriteMask = (t->has_depth && (!st->depth_set || st->depth_write))
                                              ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    gd.DepthStencilState.DepthFunc = st->depth_set ? aedx_compare_func(st->depth_op)
                                                   : D3D12_COMPARISON_FUNC_LESS;
    if (st->stencil_set) {
        /* STENCIL_* is one below D3D12_STENCIL_OP. */
        D3D12_DEPTH_STENCILOP_DESC so;
        so.StencilFailOp = (D3D12_STENCIL_OP)(st->stencil_fail + 1);
        so.StencilDepthFailOp = (D3D12_STENCIL_OP)(st->stencil_depth_fail + 1);
        so.StencilPassOp = (D3D12_STENCIL_OP)(st->stencil_pass + 1);
        so.StencilFunc = aedx_compare_func(st->stencil_op);
        gd.DepthStencilState.StencilEnable = TRUE;
        gd.DepthStencilState.StencilReadMask = (UINT8)st->stencil_read;
        gd.DepthStencilState.StencilWriteMask = (UINT8)st->stencil_write;
        gd.DepthStencilState.FrontFace = so;
        gd.DepthStencilState.BackFace = so;
        p->stencil_ref = (UINT)st->stencil_ref;
    }
    gd.InputLayout = il;
    gd.PrimitiveTopologyType = st->topology >= 4 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
                             : st->topology >= 2 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                                 : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p->prim = st->topology == 1 ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
            : st->topology == 2 ? D3D_PRIMITIVE_TOPOLOGY_LINELIST
            : st->topology == 3 ? D3D_PRIMITIVE_TOPOLOGY_LINESTRIP
            : st->topology == 4 ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    /* One render target an attachment (#2386); IndependentBlendEnable is
     * off, so render target 0's blend applies to every one. */
    gd.NumRenderTargets = (UINT)(1 + t->extra);
    gd.RTVFormats[0] = t->format;
    for (int x = 0; x < t->extra; x++) gd.RTVFormats[x + 1] = t->xformat[x];
    gd.DSVFormat = t->has_depth ? t->dsv_format : DXGI_FORMAT_UNKNOWN;
    gd.SampleDesc.Count = (UINT)t->samples;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(d->device, &gd, &IID_ID3D12PipelineState,
                                                          (void**)&p->pso);
    if (FAILED(hr)) {
        aedx_fail(AEDX_ERR_SHADER,
                  "CreateGraphicsPipelineState failed (0x%08lx): the shaders' inputs, outputs and registers "
                  "must match the layout and bindings", (unsigned long)hr);
        goto fail;
    }
    ID3D10Blob_Release(vblob);
    ID3D10Blob_Release(pblob);
    vblob = pblob = NULL;
    if (bindings && bindings->count > 0) {
        p->def = aedx_material_create(p);
        if (!p->def) goto fail;
    }
    aedx_drain_debug(d);
    return p;
fail:
    if (vblob) ID3D10Blob_Release(vblob);
    if (pblob) ID3D10Blob_Release(pblob);
    aedx_pipeline_destroy(p);
    return NULL;
}

void aedx_pipeline_destroy(AedxPipeline* p) {
    if (!p) return;
    AedxDevice* d = p->dev;
    AcquireSRWLockExclusive(&d->lock);
    aedx_idle(d);
    ReleaseSRWLockExclusive(&d->lock);
    aedx_material_destroy(p->def);
    if (p->pso) ID3D12PipelineState_Release(p->pso);
    if (p->rl.root) ID3D12RootSignature_Release(p->rl.root);
    free(p);
}

AedxMaterial* aedx_material_create(AedxPipeline* p) {
    aedx_clear_error();
    if (!p) { aedx_fail(AEDX_ERR_ARG, "pipeline is null"); return NULL; }
    int any = 0;
    for (int i = 0; i < AEDX_MAX_DESC; i++) any |= p->rl.kind[i];
    if (!any) {
        aedx_fail(AEDX_ERR_ARG, "pipeline was created without bindings, so it has no materials");
        return NULL;
    }
    AedxMaterial* m = (AedxMaterial*)calloc(1, sizeof(*m));
    if (!m) { aedx_fail(AEDX_ERR_OOM, "out of memory"); return NULL; }
    m->pipe = p;
    return m;
}

void aedx_material_destroy(AedxMaterial* m) {
    if (!m) return;
    AedxDevice* d = m->pipe->dev;
    AcquireSRWLockExclusive(&d->lock);
    aedx_idle(d);
    ReleaseSRWLockExclusive(&d->lock);
    for (int i = 0; i < AEDX_MAX_DESC; i++) {
        if (m->ub[i].res) {
            ID3D12Resource_Unmap(m->ub[i].res, 0, NULL);
            ID3D12Resource_Release(m->ub[i].res);
        }
    }
    free(m);
}

static int aedx_material_check(AedxMaterial* m, int binding, int kind, const char* what) {
    if (!m) return aedx_fail(AEDX_ERR_ARG, "material is null");
    if (binding < 0 || binding >= AEDX_MAX_DESC) {
        return aedx_fail(AEDX_ERR_ARG, "binding must be 0..%d", AEDX_MAX_DESC - 1);
    }
    int k = m->pipe->rl.kind[binding];
    if (kind == AEDX_BIND_UNIFORM && k == AEDX_BIND_UNIFORM_DYNAMIC) {
        return aedx_fail(AEDX_ERR_ARG,
                         "binding %d is a dynamic uniform: bind a buffer with set_buffer and choose "
                         "each draw's offset with batch_set_offset", binding);
    }
    if (k != kind && !(kind == AEDX_BIND_STORAGE && (k == AEDX_BIND_UNIFORM || k == AEDX_BIND_UNIFORM_DYNAMIC))) {
        return aedx_fail(AEDX_ERR_ARG, "binding %d is not declared as %s", binding, what);
    }
    return AEDX_OK;
}

/* Writes a uniform the material owns: a mapped buffer made on first use and
 * reused while big enough, so a per-frame update is a memcpy. */
int aedx_material_set_uniform(AedxMaterial* m, int binding, const void* data, size_t len) {
    aedx_clear_error();
    int rc = aedx_material_check(m, binding, AEDX_BIND_UNIFORM, "a uniform");
    if (rc != AEDX_OK) return rc;
    if (!data || len == 0) return aedx_fail(AEDX_ERR_ARG, "uniform data is empty");
    if (m->ub[binding].res && m->ub[binding].size < len) {
        AedxDevice* d = m->pipe->dev;
        AcquireSRWLockExclusive(&d->lock);
        aedx_idle(d);
        ReleaseSRWLockExclusive(&d->lock);
        ID3D12Resource_Unmap(m->ub[binding].res, 0, NULL);
        ID3D12Resource_Release(m->ub[binding].res);
        memset(&m->ub[binding], 0, sizeof(m->ub[binding]));
    }
    if (!m->ub[binding].res) {
        size_t bytes = (len + 255u) & ~(size_t)255u;   /* a CBV's size is a multiple of 256 */
        ID3D12Resource* r = aedx_make_buffer(m->pipe->dev, D3D12_HEAP_TYPE_UPLOAD, bytes,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
        if (!r) return AEDX_ERR_OOM;
        D3D12_RANGE none = { 0, 0 };
        HRESULT hr = ID3D12Resource_Map(r, 0, &none, (void**)&m->ub[binding].ptr);
        if (FAILED(hr)) {
            ID3D12Resource_Release(r);
            return aedx_fail(AEDX_ERR_OOM, "Map (uniform) failed (0x%08lx)", (unsigned long)hr);
        }
        memset(m->ub[binding].ptr, 0, bytes);
        m->ub[binding].res = r;
        m->ub[binding].size = len;
    }
    memcpy(m->ub[binding].ptr, data, len);
    m->buf[binding] = NULL;
    m->set[binding] = 1;
    return AEDX_OK;
}

int aedx_material_set_texture(AedxMaterial* m, int binding, AedxTexture* tex) {
    aedx_clear_error();
    int rc = aedx_material_check(m, binding, AEDX_BIND_TEXTURE, "a texture");
    if (rc != AEDX_OK) return rc;
    if (!tex) return aedx_fail(AEDX_ERR_ARG, "texture is null");
    rc = aedx_texture_ready(tex);
    if (rc != AEDX_OK) return rc;
    m->tex[binding] = tex;
    m->tgt[binding] = NULL;
    m->set[binding] = 1;
    return AEDX_OK;
}

/* From now on the target's depth is a typeless resource with a shader view:
 * new images, made once, the first time a material binds the depth. */
static int aedx_target_enable_depth_sampling(AedxTarget* t) {
    if (t->depth_sampled) return AEDX_OK;
    AedxDevice* d = t->dev;
    AcquireSRWLockExclusive(&d->lock);
    aedx_wait_all_locked(t);
    int frames = t->frame_count > 0 ? t->frame_count : 1;
    aedx_frames_free(t);
    ReleaseSRWLockExclusive(&d->lock);
    aedx_target_free_images(t);
    t->depth_sampled = 1;
    int rc = aedx_target_make_images(t);
    if (rc == AEDX_OK) {
        AcquireSRWLockExclusive(&d->lock);
        rc = aedx_frames_alloc(t, frames);
        ReleaseSRWLockExclusive(&d->lock);
    }
    if (rc != AEDX_OK) aedx_target_free_images(t);
    return rc;
}

/* A target's newest frame, its colour or its depth, where a texture goes
 * (#2198). The frame read is the one most recently submitted to the target
 * when the draw runs on the queue. The target must outlive every draw that
 * uses the material. */
static int aedx_material_set_target_view(AedxMaterial* m, int binding, AedxTarget* tg, int depth,
                                        int compare, int att, int view) {
    aedx_clear_error();
    int rc = aedx_material_check(m, binding, AEDX_BIND_TEXTURE, "a texture");
    if (rc != AEDX_OK) return rc;
    if (!tg) return aedx_fail(AEDX_ERR_ARG, "target is null");
    if (tg->dev != m->pipe->dev) return aedx_fail(AEDX_ERR_ARG, "the target belongs to another device");
    /* A layered target is sampled whole, as an array or a cube (#2399). */
    if (tg->layered && !view) {
        return aedx_fail(AEDX_ERR_ARG, "a layered target is sampled with set_target_array or set_target_cube");
    }
    if (view && !tg->layered) {
        return aedx_fail(AEDX_ERR_ARG, "set_target_array and set_target_cube sample a target made by "
                         "target_create_layered");
    }
    if (view == 2 && !tg->cube) {
        return aedx_fail(AEDX_ERR_ARG, "the target is not a cube: sample it with set_target_array");
    }
    if (depth) {
        if (!tg->has_depth) return aedx_fail(AEDX_ERR_ARG, "the target was created without depth");
        if (tg->samples > 1) {
            return aedx_fail(AEDX_ERR_UNSUPPORTED,
                             "a multisampled target's depth cannot be sampled as a texture");
        }
        rc = aedx_target_enable_depth_sampling(tg);
        if (rc != AEDX_OK) return rc;
        if (compare && tg->compare_slot[compare] < 0) {
            rc = aedx_make_sampler_ex(tg->dev, 1, compare, &tg->compare_slot[compare]);
            if (rc != AEDX_OK) return rc;
        }
    }
    if (!depth && (att < 0 || att > tg->extra)) {
        return aedx_fail(AEDX_ERR_ARG, "the target has colour attachments 0..%d, not %d", tg->extra, att);
    }
    if (!tg->color) return aedx_fail(AEDX_ERR_ARG, "target has no images: its last resize failed");
    m->tgt[binding] = tg;
    m->tgt_depth[binding] = depth ? 1 : 0;
    m->tgt_cmp[binding] = depth ? compare : 0;
    m->tgt_att[binding] = depth ? 0 : att;
    m->tgt_view[binding] = view;
    m->tex[binding] = NULL;
    m->set[binding] = 1;
    return AEDX_OK;
}

static int aedx_material_set_target_att(AedxMaterial* m, int binding, AedxTarget* tg, int depth,
                                       int compare, int att) {
    return aedx_material_set_target_view(m, binding, tg, depth, compare, att, 0);
}

static int aedx_material_set_target_cmp(AedxMaterial* m, int binding, AedxTarget* tg, int depth,
                                       int compare) {
    return aedx_material_set_target_att(m, binding, tg, depth, compare, 0);
}

/* Every slice of a layered target as a Texture2DArray, or a cube target's
 * faces as a TextureCube (#2399). */
int aedx_material_set_target_array(AedxMaterial* m, int binding, AedxTarget* tg) {
    return aedx_material_set_target_view(m, binding, tg, 0, 0, 0, 1);
}

int aedx_material_set_target_cube(AedxMaterial* m, int binding, AedxTarget* tg) {
    return aedx_material_set_target_view(m, binding, tg, 0, 0, 0, 2);
}

/* A layered target's depth as a Texture2DArray or a cube's as a
 * TextureCube (#2412): raw with `op` 0, or through a comparison sampler. */
static int aedx_material_set_target_depth_view(AedxMaterial* m, int binding, AedxTarget* tg, int op, int view) {
    if (op < 0 || op >= AEDX_COMPARE_OPS) {
        aedx_clear_error();
        return aedx_fail(AEDX_ERR_ARG, "op %d is not 0 (raw) or COMPARE_LESS..COMPARE_NEVER (1..8)", op);
    }
    return aedx_material_set_target_view(m, binding, tg, 1, op, 0, view);
}

int aedx_material_set_target_depth_array(AedxMaterial* m, int binding, AedxTarget* tg, int op) {
    return aedx_material_set_target_depth_view(m, binding, tg, op, 1);
}

int aedx_material_set_target_depth_cube(AedxMaterial* m, int binding, AedxTarget* tg, int op) {
    return aedx_material_set_target_depth_view(m, binding, tg, op, 2);
}

int aedx_pipeline_set_target_depth_array(AedxPipeline* p, int binding, AedxTarget* tg, int op) {
    aedx_clear_error();
    if (!p || !p->def) return aedx_fail(AEDX_ERR_ARG, "pipeline is null or has no bindings");
    return aedx_material_set_target_depth_array(p->def, binding, tg, op);
}

int aedx_pipeline_set_target_depth_cube(AedxPipeline* p, int binding, AedxTarget* tg, int op) {
    aedx_clear_error();
    if (!p || !p->def) return aedx_fail(AEDX_ERR_ARG, "pipeline is null or has no bindings");
    return aedx_material_set_target_depth_cube(p->def, binding, tg, op);
}

int aedx_pipeline_set_target_array(AedxPipeline* p, int binding, AedxTarget* tg) {
    aedx_clear_error();
    if (!p || !p->def) return aedx_fail(AEDX_ERR_ARG, "pipeline is null or has no bindings");
    return aedx_material_set_target_array(p->def, binding, tg);
}

int aedx_pipeline_set_target_cube(AedxPipeline* p, int binding, AedxTarget* tg) {
    aedx_clear_error();
    if (!p || !p->def) return aedx_fail(AEDX_ERR_ARG, "pipeline is null or has no bindings");
    return aedx_material_set_target_cube(p->def, binding, tg);
}

int aedx_material_set_target(AedxMaterial* m, int binding, AedxTarget* tg, int depth) {
    return aedx_material_set_target_cmp(m, binding, tg, depth, 0);
}

/* Colour attachment `n` of a target's newest frame where a texture goes
 * (#2386); attachment 0 is what set_target binds. */
int aedx_material_set_target_attachment(AedxMaterial* m, int binding, AedxTarget* tg, int n) {
    return aedx_material_set_target_att(m, binding, tg, 0, 0, n);
}

int aedx_pipeline_set_target_attachment(AedxPipeline* p, int binding, AedxTarget* tg, int n) {
    aedx_clear_error();
    if (!p || !p->def) return aedx_fail(AEDX_ERR_ARG, "pipeline is null or has no bindings");
    return aedx_material_set_target_attachment(p->def, binding, tg, n);
}

int aedx_target_attachments(const AedxTarget* t) { return t ? 1 + t->extra : 0; }

/* The slice of a layered target the next draws render into (#2399). Lists
 * are recorded fresh each frame, so the next one simply uses its views. */
int aedx_target_set_layer(AedxTarget* t, int layer) {
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    if (!t->layered) return aedx_fail(AEDX_ERR_ARG, "the target is not layered: make it with target_create_layered");
    if (layer < 0 || layer >= t->layers) {
        return aedx_fail(AEDX_ERR_ARG, "the target has layers 0..%d, not %d", t->layers - 1, layer);
    }
    AcquireSRWLockExclusive(&t->dev->lock);
    t->layer = layer;
    ReleaseSRWLockExclusive(&t->dev->lock);
    return AEDX_OK;
}

int aedx_target_layers(const AedxTarget* t) { return t ? t->layers : 0; }

/* Draw `item`'s own pipeline (#2411), as contrib.vulkan's. */
int aedx_batch_set_pipeline(AedxTarget* t, int item, AedxPipeline* p) {
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    if (p && p->target != t) return aedx_fail(AEDX_ERR_ARG, "the pipeline was made for another target");
    if (item < 0 || item >= t->batch_count) {
        return aedx_fail(AEDX_ERR_ARG, "draw %d is outside the batch of %d", item, t->batch_count);
    }
    t->batch[item].pipe = p;
    return AEDX_OK;
}

/* A target's depth read through a comparison sampler (#2373): a shadow map.
 * The shader (Texture2D.SampleCmp with a SamplerComparisonState) gets the
 * fraction of the footprint whose depth passes `reference op texel`. */
int aedx_material_set_target_depth_compare(AedxMaterial* m, int binding, AedxTarget* tg, int op) {
    if (op < 1 || op >= AEDX_COMPARE_OPS) {
        return aedx_fail(AEDX_ERR_ARG, "compare op %d is not COMPARE_LESS..COMPARE_NEVER (1..8)", op);
    }
    return aedx_material_set_target_cmp(m, binding, tg, 1, op);
}

int aedx_pipeline_set_target_depth_compare(AedxPipeline* p, int binding, AedxTarget* tg, int op) {
    aedx_clear_error();
    if (!p || !p->def) return aedx_fail(AEDX_ERR_ARG, "pipeline is null or has no bindings");
    return aedx_material_set_target_depth_compare(p->def, binding, tg, op);
}

/* R32_FLOAT comparison filtering is required at every feature level the
 * module runs on, so a depth target always filters. */
int aedx_target_depth_linear(const AedxTarget* t) { return t && t->has_depth ? 1 : 0; }

int aedx_material_set_buffer(AedxMaterial* m, int binding, AedxBuffer* buf) {
    aedx_clear_error();
    int rc = aedx_material_check(m, binding, AEDX_BIND_STORAGE, "a storage or uniform buffer");
    if (rc != AEDX_OK) return rc;
    if (!buf) return aedx_fail(AEDX_ERR_ARG, "buffer is null");
    if (buf->dev != m->pipe->dev) return aedx_fail(AEDX_ERR_ARG, "the buffer belongs to another device");
    const AedxRootLayout* rl = &m->pipe->rl;
    if (rl->kind[binding] == AEDX_BIND_UNIFORM_DYNAMIC && buf->size < rl->dyn_range[binding]) {
        return aedx_fail(AEDX_ERR_ARG, "a %zu-byte buffer is smaller than binding %d's %u-byte window",
                         buf->size, binding, rl->dyn_range[binding]);
    }
    m->buf[binding] = buf;
    m->set[binding] = 1;
    return AEDX_OK;
}

/* ------------------------------------------------------------------------ */
/* Drawing                                                                   */
/* ------------------------------------------------------------------------ */

/* Every binding the pipeline declares has to hold something before a draw
 * reads it: a root descriptor with no address is a device removal, not a
 * black pixel. */
static int aedx_material_ready(const AedxPipeline* p, const AedxMaterial* m) {
    for (int i = 0; i < AEDX_MAX_DESC; i++) {
        if (p->rl.kind[i] && (!m || !m->set[i])) {
            return aedx_fail(AEDX_ERR_ARG, "binding %d was declared but never set", i);
        }
    }
    return AEDX_OK;
}

static void aedx_bind_material(AedxDevice* d, ID3D12GraphicsCommandList* list, const AedxRootLayout* rl,
                               const AedxMaterial* m, int compute, const UINT* offsets) {
    for (int i = 0; i < AEDX_MAX_DESC; i++) {
        int k = rl->kind[i];
        if (!k) continue;
        UINT param = (UINT)rl->param[i];
        if (k == AEDX_BIND_STORAGE_TEXTURE) {
            /* Compute only: make_root refuses it for a draw. */
            ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, param,
                                                                    aedx_slot_gpu(&d->srv, m->tex[i]->uav_slot));
            continue;
        }
        if (k == AEDX_BIND_TEXTURE) {
            int srv_slot, smp_slot;
            if (m->tgt[i]) {
                const AedxTarget* tg = m->tgt[i];
                int a = m->tgt_att[i];
                int view = m->tgt_view[i];
                if (m->tgt_depth[i]) {
                    srv_slot = view == 2 ? tg->depth_cube_srv_slot : tg->depth_srv_slot;
                } else {
                    srv_slot = view == 2 ? tg->cube_srv_slot : a ? tg->xsrv_slot[a - 1] : tg->srv_slot;
                }
                smp_slot = m->tgt_cmp[i] ? tg->compare_slot[m->tgt_cmp[i]]
                         : m->tgt_depth[i] ? tg->depth_sampler_slot
                         : a ? tg->xsampler_slot[a - 1] : tg->sampler_slot;
            } else {
                srv_slot = m->tex[i]->srv_slot;
                smp_slot = m->tex[i]->sampler_slot;
            }
            D3D12_GPU_DESCRIPTOR_HANDLE srv = aedx_slot_gpu(&d->srv, srv_slot);
            D3D12_GPU_DESCRIPTOR_HANDLE smp = aedx_slot_gpu(&d->samplers, smp_slot);
            if (compute) {
                ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, param, srv);
                ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, param + 1, smp);
            } else {
                ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, param, srv);
                ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, param + 1, smp);
            }
            continue;
        }
        D3D12_GPU_VIRTUAL_ADDRESS va = m->buf[i] ? ID3D12Resource_GetGPUVirtualAddress(m->buf[i]->res)
                                                 : ID3D12Resource_GetGPUVirtualAddress(m->ub[i].res);
        /* A dynamic uniform's window starts at the draw's offset (#2198). */
        if (k == AEDX_BIND_UNIFORM_DYNAMIC && offsets) va += offsets[i];
        if (k == AEDX_BIND_UNIFORM || k == AEDX_BIND_UNIFORM_DYNAMIC) {
            if (compute) ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(list, param, va);
            else ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, param, va);
        } else {
            if (compute) ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, param, va);
            else ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(list, param, va);
        }
    }
}

/* A target a frame samples, and which of its images (#2198). */
typedef struct {
    AedxTarget* tg;
    int         depth;
    int         att;     /* the colour attachment, when not depth (#2386) */
} AedxSampled;

#define AEDX_MAX_SAMPLED 32

/* Adds the targets one material samples, each image once, after checking it
 * can be read: not the target being drawn, and drawn at least once. */
static int aedx_collect_sampled(AedxTarget* t, const AedxMaterial* m, AedxSampled* out, int* n) {
    if (!m) return AEDX_OK;
    for (int b = 0; b < AEDX_MAX_DESC; b++) {
        AedxTarget* tg = m->tgt[b];
        if (!tg) continue;
        if (tg == t) {
            return aedx_fail(AEDX_ERR_ARG,
                             "binding %d samples the target being drawn; draw into another target", b);
        }
        if (!tg->color || !tg->rendered) {
            return aedx_fail(AEDX_ERR_ARG,
                             "binding %d samples a target that has no frame yet: draw it first", b);
        }
        int seen = 0;
        for (int i = 0; i < *n; i++) {
            if (out[i].tg == tg && out[i].depth == m->tgt_depth[b] && out[i].att == m->tgt_att[b]) {
                seen = 1;
                break;
            }
        }
        if (seen) continue;
        if (*n >= AEDX_MAX_SAMPLED) {
            return aedx_fail(AEDX_ERR_ARG, "a frame samples more than %d target images", AEDX_MAX_SAMPLED);
        }
        out[*n].tg = tg;
        out[*n].depth = m->tgt_depth[b];
        out[*n].att = m->tgt_att[b];
        (*n)++;
    }
    return AEDX_OK;
}

/* Moves each sampled image into a shader-readable state before the frame's
 * draws (`to_read`) and back to its resting state after them. Recording
 * happens under the device lock right before execution, so the state tracked
 * here is the state the queue will see. */
static void aedx_sampled_barriers(ID3D12GraphicsCommandList* l, AedxSampled* list, int n, int to_read) {
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    for (int i = 0; i < n; i++) {
        AedxTarget* tg = list[i].tg;
        if (list[i].depth) {
            if (to_read) {
                aedx_barrier(l, tg->depth, tg->depth_state, read);
                tg->depth_state = read;
            } else {
                aedx_barrier(l, tg->depth, tg->depth_state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                tg->depth_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            }
        } else {
            int a = list[i].att;
            ID3D12Resource* img = a ? tg->xcolor[a - 1] : tg->color;
            D3D12_RESOURCE_STATES* st = a ? &tg->xstate[a - 1] : &tg->color_state;
            /* COPY_SOURCE is where a target's colour rests between frames:
             * readback and presenting start from it. */
            D3D12_RESOURCE_STATES want = to_read ? read : D3D12_RESOURCE_STATE_COPY_SOURCE;
            aedx_barrier(l, img, *st, want);
            *st = want;
        }
    }
}

/* The command signature an indirect draw of the target's kind uses, made on
 * first use (#2198). THE DEVICE LOCK IS HELD. */
static int aedx_indirect_signature(AedxDevice* d, int indexed) {
    ID3D12CommandSignature** slot = indexed ? &d->sig_indexed : &d->sig_draw;
    if (*slot) return AEDX_OK;
    D3D12_INDIRECT_ARGUMENT_DESC arg;
    memset(&arg, 0, sizeof(arg));
    arg.Type = indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.ByteStride = indexed ? AEDX_INDIRECT_INDEXED_BYTES : AEDX_INDIRECT_BYTES;
    sd.NumArgumentDescs = 1;
    sd.pArgumentDescs = &arg;
    HRESULT hr = ID3D12Device_CreateCommandSignature(d->device, &sd, NULL, &IID_ID3D12CommandSignature,
                                                     (void**)slot);
    if (FAILED(hr)) {
        *slot = NULL;
        return aedx_fail(aedx_hr_status(hr), "CreateCommandSignature failed (0x%08lx)", (unsigned long)hr);
    }
    return AEDX_OK;
}

/* Records one draw: a range of the target's geometry, instanced, or the
 * commands an indirect draw reads, all in one ExecuteIndirect (#2198). */
static void aedx_record_draw(AedxDevice* d, AedxTarget* t, ID3D12GraphicsCommandList* l, const AedxDrawItem* it) {
    int indexed = t->index_count > 0;
    if (it->indirect) {
        ID3D12GraphicsCommandList_ExecuteIndirect(l, indexed ? d->sig_indexed : d->sig_draw,
                                                  (UINT)it->indirect_draws, it->indirect->res,
                                                  it->indirect_offset, NULL, 0);
        return;
    }
    UINT instances = (UINT)(it->instances > 0 ? it->instances : t->instance_count);
    UINT first_instance = (UINT)(it->instances > 0 ? it->first_instance : 0);
    if (indexed) {
        ID3D12GraphicsCommandList_DrawIndexedInstanced(l, (UINT)it->count, instances, (UINT)it->first, 0,
                                                       first_instance);
    } else {
        ID3D12GraphicsCommandList_DrawInstanced(l, (UINT)it->count, instances, (UINT)it->first, first_instance);
    }
}

/* Sets a pipeline inside the frame with what it reads besides its
 * bindings: its root signature, topology, push constants sized to its range
 * so a caller who set fewer bytes still gets a defined block, and the vertex
 * streams. */
static void aedx_record_pipeline(AedxTarget* t, ID3D12GraphicsCommandList* l, AedxPipeline* p) {
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(l, p->rl.root);
    ID3D12GraphicsCommandList_SetPipelineState(l, p->pso);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(l, p->prim);
    if (p->rl.push_words > 0) {
        unsigned char block[AEDX_MAX_PUSH];
        memset(block, 0, sizeof(block));
        UINT n = t->push_size < (UINT)p->push_bytes ? t->push_size : (UINT)p->push_bytes;
        if (n) memcpy(block, t->push, n);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, 0, p->rl.push_words, block, 0);
    }
    if (p->vertex_input) {
        D3D12_VERTEX_BUFFER_VIEW vbv;
        vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(t->vbuf);
        vbv.SizeInBytes = (UINT)(t->vertex_count * t->vertex_floats * 4);
        vbv.StrideInBytes = p->stride ? p->stride : (UINT)(t->vertex_floats * 4);
        ID3D12GraphicsCommandList_IASetVertexBuffers(l, 0, 1, &vbv);
    }
    /* The caller's streams past slot 0, which the draw check made sure are
     * bound (#2198). */
    for (UINT sb = 1; sb < AEDX_MAX_BINDINGS; sb++) {
        if (!p->stream_declared[sb]) continue;
        D3D12_VERTEX_BUFFER_VIEW sv;
        sv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(t->streams[sb]->res);
        sv.SizeInBytes = (UINT)t->streams[sb]->size;
        sv.StrideInBytes = p->stream_stride[sb];
        ID3D12GraphicsCommandList_IASetVertexBuffers(l, sb, 1, &sv);
    }
}

/* Records one frame into slot `fr`. Direct3D 12 lists are recorded fresh
 * every frame: recording is cheap, and a list re-executed with a stale state
 * assumption would be the kind of bug caching invites. */
static int aedx_record(AedxTarget* t, AedxFrame* fr, AedxPipeline* p, AedxMaterial* mat, const float clear[4]) {
    AedxDevice* d = t->dev;
    /* What this frame samples, checked before anything is recorded. */
    AedxSampled sampled[AEDX_MAX_SAMPLED];
    int n_sampled = 0;
    if (p && t->vertex_count > 0) {
        AedxMaterial* use = mat ? mat : p->def;
        int src = AEDX_OK;
        if (t->batch_count > 0) {
            for (int i = 0; i < t->batch_count && src == AEDX_OK; i++) {
                AedxDrawItem* it = &t->batch[i];
                src = aedx_collect_sampled(t, it->mat ? it->mat : it->pipe ? it->pipe->def : use, sampled,
                                           &n_sampled);
            }
        } else {
            src = aedx_collect_sampled(t, use, sampled, &n_sampled);
        }
        if (src != AEDX_OK) return src;
    }

    HRESULT hr = ID3D12CommandAllocator_Reset(fr->alloc);
    if (SUCCEEDED(hr)) hr = ID3D12GraphicsCommandList_Reset(fr->list, fr->alloc, NULL);
    if (FAILED(hr)) return aedx_fail(aedx_hr_status(hr), "cannot reset the command list (0x%08lx)", (unsigned long)hr);
    ID3D12GraphicsCommandList* l = fr->list;
    /* The frame's first timestamp; its pair and the resolve close the list. */
    UINT timer = (UINT)(fr - t->frames) * 2u;
    fr->timed = t->timing_on && t->timer_heap;
    if (fr->timed) ID3D12GraphicsCommandList_EndQuery(l, t->timer_heap, D3D12_QUERY_TYPE_TIMESTAMP, timer);
    aedx_sampled_barriers(l, sampled, n_sampled, 1);

    /* Each attachment's view: the multisampled one when there is one,
     * which resolves after the draws, otherwise the image itself. */
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[AEDX_MAX_COLOR];
    D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(t->rtv_heap);
    for (int c = 0; c <= t->extra; c++) {
        rtvs[c] = rtv0;
        rtvs[c].ptr += (SIZE_T)(2 * c + (t->samples > 1 ? 1 : 0)) * d->rtv_inc;
    }
    /* A layered target's views are a slice each (#2399), and a multisampled
     * one draws into its one multisampled image, after them (#2412). */
    if (t->layered) {
        rtvs[0].ptr = rtv0.ptr + (SIZE_T)(t->samples > 1 ? t->layers : t->layer) * d->rtv_inc;
    }
    if (t->samples == 1) {
        aedx_barrier(l, t->color, t->color_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        t->color_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        for (int x = 0; x < t->extra; x++) {
            aedx_barrier(l, t->xcolor[x], t->xstate[x], D3D12_RESOURCE_STATE_RENDER_TARGET);
            t->xstate[x] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
    }
    /* A new layered target's first frame clears every slice: colour 0, and
     * depth at the far plane, so a slice never drawn reads defined values
     * (#2412). */
    if (t->layered && t->layers_fresh) {
        const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        aedx_barrier(l, t->color, t->color_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        t->color_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        for (int s = 0; s < t->layers; s++) {
            D3D12_CPU_DESCRIPTOR_HANDLE r = rtv0;
            r.ptr += (SIZE_T)s * d->rtv_inc;
            ID3D12GraphicsCommandList_ClearRenderTargetView(l, r, zero, 0, NULL);
            if (t->has_depth && t->samples == 1) {
                D3D12_CPU_DESCRIPTOR_HANDLE z = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(t->dsv_heap);
                z.ptr += (SIZE_T)s * d->dsv_inc;
                ID3D12GraphicsCommandList_ClearDepthStencilView(l, z, D3D12_CLEAR_FLAG_DEPTH |
                                                                (t->has_stencil ? D3D12_CLEAR_FLAG_STENCIL : 0),
                                                                1.0f, 0, 0, NULL);
            }
        }
        t->layers_fresh = 0;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    dsv.ptr = 0;
    if (t->has_depth) {
        dsv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(t->dsv_heap);
        if (t->layered && t->samples == 1) dsv.ptr += (SIZE_T)t->layer * d->dsv_inc;
        /* The stencil clears to 0 with the depth (#2399). */
        ID3D12GraphicsCommandList_ClearDepthStencilView(l, dsv, D3D12_CLEAR_FLAG_DEPTH |
                                                        (t->has_stencil ? D3D12_CLEAR_FLAG_STENCIL : 0),
                                                        1.0f, 0, 0, NULL);
    }
    for (int c = 0; c <= t->extra; c++) ID3D12GraphicsCommandList_ClearRenderTargetView(l, rtvs[c], clear, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(l, (UINT)(1 + t->extra), rtvs, FALSE, t->has_depth ? &dsv : NULL);

    if (p && t->vertex_count > 0) {
        D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)t->width, (float)t->height, 0.0f, 1.0f };
        D3D12_RECT sc = { 0, 0, t->width, t->height };
        ID3D12GraphicsCommandList_RSSetViewports(l, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
        ID3D12DescriptorHeap* heaps[2] = { d->srv.heap, d->samplers.heap };
        ID3D12GraphicsCommandList_SetDescriptorHeaps(l, 2, heaps);
        aedx_record_pipeline(t, l, p);
        if (t->index_count > 0) {
            D3D12_INDEX_BUFFER_VIEW ibv;
            ibv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(t->ibuf);
            ibv.SizeInBytes = (UINT)(t->index_count * (t->index_bits / 8));
            ibv.Format = t->index_bits == 16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
            ID3D12GraphicsCommandList_IASetIndexBuffer(l, &ibv);
        }
        AedxMaterial* bind_mat = mat ? mat : p->def;
        if (t->batch_count > 0) {
            AedxPipeline* bound = p;
            for (int i = 0; i < t->batch_count; i++) {
                AedxDrawItem* it = &t->batch[i];
                /* A draw's own pipeline is set for it, and the frame's again
                 * after it (#2411). */
                AedxPipeline* ip = it->pipe ? it->pipe : p;
                if (ip != bound) {
                    aedx_record_pipeline(t, l, ip);
                    bound = ip;
                }
                AedxMaterial* im = it->mat ? it->mat : it->pipe ? ip->def : bind_mat;
                if (im) aedx_bind_material(d, l, &ip->rl, im, 0, it->offsets);
                /* Each draw's own scissor and viewport, or the whole target
                 * (#2398). */
                D3D12_VIEWPORT ivp = vp;
                D3D12_RECT isc = sc;
                if (it->viewport_set) {
                    ivp.TopLeftX = it->vx; ivp.TopLeftY = it->vy; ivp.Width = it->vw; ivp.Height = it->vh;
                    ivp.MinDepth = it->vmin; ivp.MaxDepth = it->vmax;
                }
                if (it->scissor_set) {
                    isc.left = it->sx; isc.top = it->sy;
                    isc.right = it->sx + it->sw; isc.bottom = it->sy + it->sh;
                }
                ID3D12GraphicsCommandList_RSSetViewports(l, 1, &ivp);
                ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &isc);
                ID3D12GraphicsCommandList_OMSetStencilRef(l, it->stencil_ref_set ? it->stencil_ref : ip->stencil_ref);
                aedx_record_draw(d, t, l, it);
            }
        } else {
            if (bind_mat) aedx_bind_material(d, l, &p->rl, bind_mat, 0, NULL);
            AedxDrawItem all;
            memset(&all, 0, sizeof(all));
            all.count = t->index_count > 0 ? t->index_count : t->vertex_count;
            ID3D12GraphicsCommandList_OMSetStencilRef(l, p->stencil_ref);
            aedx_record_draw(d, t, l, &all);
        }
    }

    aedx_sampled_barriers(l, sampled, n_sampled, 0);

    if (t->samples > 1) {
        aedx_barrier(l, t->msaa, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        aedx_barrier(l, t->color, t->color_state, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        /* Into the slice drawn, for a layered target (#2412). */
        ID3D12GraphicsCommandList_ResolveSubresource(l, t->color, (UINT)t->layer, t->msaa, 0, t->format);
        aedx_barrier(l, t->msaa, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        aedx_barrier(l, t->color, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (int x = 0; x < t->extra; x++) {
            aedx_barrier(l, t->xmsaa[x], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            aedx_barrier(l, t->xcolor[x], t->xstate[x], D3D12_RESOURCE_STATE_RESOLVE_DEST);
            ID3D12GraphicsCommandList_ResolveSubresource(l, t->xcolor[x], 0, t->xmsaa[x], 0, t->xformat[x]);
            aedx_barrier(l, t->xmsaa[x], D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            aedx_barrier(l, t->xcolor[x], D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
    } else {
        aedx_barrier(l, t->color, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (int x = 0; x < t->extra; x++) {
            aedx_barrier(l, t->xcolor[x], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
    }
    t->color_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    for (int x = 0; x < t->extra; x++) t->xstate[x] = D3D12_RESOURCE_STATE_COPY_SOURCE;

    if (t->readback_on) {
        D3D12_TEXTURE_COPY_LOCATION dst, src;
        memset(&dst, 0, sizeof(dst));
        memset(&src, 0, sizeof(src));
        dst.pResource = fr->readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = t->footprint;
        src.pResource = t->color;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = (UINT)t->layer;   /* the slice drawn (#2399) */
        ID3D12GraphicsCommandList_CopyTextureRegion(l, &dst, 0, 0, 0, &src, NULL);
        for (int x = 0; x < t->extra; x++) {
            dst.pResource = fr->xreadback[x];
            dst.PlacedFootprint = t->xfootprint[x];
            src.pResource = t->xcolor[x];
            ID3D12GraphicsCommandList_CopyTextureRegion(l, &dst, 0, 0, 0, &src, NULL);
        }
    }
    if (fr->timed) {
        ID3D12GraphicsCommandList_EndQuery(l, t->timer_heap, D3D12_QUERY_TYPE_TIMESTAMP, timer + 1u);
        ID3D12GraphicsCommandList_ResolveQueryData(l, t->timer_heap, D3D12_QUERY_TYPE_TIMESTAMP, timer, 2,
                                                   t->timer_rb, (UINT64)timer * sizeof(UINT64));
    }
    hr = ID3D12GraphicsCommandList_Close(l);
    if (FAILED(hr)) {
        aedx_drain_debug(d);
        return aedx_fail(AEDX_ERR_ARG, "the frame's commands were invalid (0x%08lx)", (unsigned long)hr);
    }
    return AEDX_OK;
}

/* The dynamic uniforms a draw with material `m` and `offsets` (NULL: all 0)
 * reads are in range, and it sets no offset the pipeline has no dynamic
 * binding for (#2198). */
static int aedx_offsets_check(const AedxPipeline* p, const AedxMaterial* m, const UINT* offsets, int item) {
    for (int b = 0; b < AEDX_MAX_DESC; b++) {
        UINT off = offsets ? offsets[b] : 0;
        if (p->rl.kind[b] != AEDX_BIND_UNIFORM_DYNAMIC) {
            if (off != 0) {
                return aedx_fail(AEDX_ERR_ARG,
                                 "draw %d sets an offset for binding %d, which the pipeline does not "
                                 "declare as a dynamic uniform", item, b);
            }
            continue;
        }
        const AedxBuffer* buf = m ? m->buf[b] : NULL;
        if (!buf) {
            return aedx_fail(AEDX_ERR_ARG,
                             "binding %d is a dynamic uniform with no buffer bound: set_buffer first", b);
        }
        if ((size_t)off + p->rl.dyn_range[b] > buf->size) {
            return aedx_fail(AEDX_ERR_ARG,
                             "draw %d reads binding %d's %u bytes at offset %u, past the %zu-byte buffer",
                             item, b, p->rl.dyn_range[b], off, buf->size);
        }
    }
    return AEDX_OK;
}

/* Every stream the pipeline declares past slot 0 has a buffer holding what
 * the frame's draws read from it (#2198). */
static int aedx_streams_check(const AedxTarget* t, const AedxPipeline* p) {
    long long instances_end = t->batch_count == 0 ? (long long)t->instance_count : 0;
    for (int i = 0; i < t->batch_count; i++) {
        const AedxDrawItem* it = &t->batch[i];
        if (it->indirect) continue;
        long long end = it->instances > 0 ? (long long)it->first_instance + it->instances
                                          : (long long)t->instance_count;
        if (end > instances_end) instances_end = end;
    }
    for (int b = 1; b < AEDX_MAX_BINDINGS; b++) {
        if (!p->stream_declared[b]) continue;
        const AedxBuffer* buf = t->streams[b];
        if (!buf) {
            return aedx_fail(AEDX_ERR_ARG,
                             "the pipeline reads vertex stream %d, but no buffer is bound to it "
                             "(vertex_stream)", b);
        }
        long long n = p->stream_instanced[b] ? instances_end : (long long)t->vertex_count;
        unsigned long long need = (unsigned long long)n * p->stream_stride[b];
        if (need > (unsigned long long)buf->size) {
            return aedx_fail(AEDX_ERR_ARG,
                             "vertex stream %d needs %llu bytes for %lld %s, but its buffer has %zu",
                             b, need, n, p->stream_instanced[b] ? "instances" : "vertices", buf->size);
        }
    }
    return AEDX_OK;
}

/* THE DEVICE LOCK IS HELD. Records into the next slot and submits it
 * without waiting; returns the slot or a negative status. */
static int aedx_submit_locked(AedxTarget* t, AedxPipeline* p, AedxMaterial* mat, const float clear[4]) {
    if (!t->color || t->frame_count == 0) {
        return aedx_fail(AEDX_ERR_ARG, "target has no images: its last resize failed");
    }
    if (p && t->vertex_count > 0) {
        if (p->vertex_input && !t->vbuf) return aedx_fail(AEDX_ERR_ARG, "vertices were never uploaded");
        int limit = t->index_count > 0 ? t->index_count : t->vertex_count;
        UINT64 cmd_bytes = t->index_count > 0 ? AEDX_INDIRECT_INDEXED_BYTES : AEDX_INDIRECT_BYTES;
        for (int i = 0; i < t->batch_count; i++) {
            AedxDrawItem* it = &t->batch[i];
            if (it->indirect) {
                UINT64 end = it->indirect_offset + (UINT64)it->indirect_draws * cmd_bytes;
                if (end > (UINT64)it->indirect->size) {
                    return aedx_fail(AEDX_ERR_ARG,
                                     "draw %d reads %d commands of %u bytes at %llu, past the %zu-byte buffer",
                                     i, it->indirect_draws, (unsigned)cmd_bytes,
                                     (unsigned long long)it->indirect_offset, it->indirect->size);
                }
                int src = aedx_indirect_signature(t->dev, t->index_count > 0);
                if (src != AEDX_OK) return src;
            } else if ((long long)it->first + it->count > limit) {
                return aedx_fail(AEDX_ERR_ARG, "draw %d covers %d..%lld but only %d are uploaded",
                                 i, it->first, (long long)it->first + it->count - 1, limit);
            }
            /* A draw with its own pipeline is checked against it (#2411). */
            AedxPipeline* ip = it->pipe ? it->pipe : p;
            if (it->mat && it->mat->pipe != ip) {
                return aedx_fail(AEDX_ERR_ARG, "draw %d uses a material of another pipeline", i);
            }
            if (it->pipe) {
                if (ip->vertex_input && !t->vbuf) return aedx_fail(AEDX_ERR_ARG, "vertices were never uploaded");
                int src = aedx_streams_check(t, ip);
                if (src != AEDX_OK) return src;
            }
            if (it->scissor_set && ((long long)it->sx + it->sw > t->width || (long long)it->sy + it->sh > t->height)) {
                return aedx_fail(AEDX_ERR_ARG, "draw %d's scissor %d,%d %dx%d runs outside the %dx%d target",
                                 i, it->sx, it->sy, it->sw, it->sh, t->width, t->height);
            }
            AedxMaterial* im = it->mat ? it->mat : it->pipe ? ip->def : (mat ? mat : p->def);
            int rc = aedx_material_ready(ip, im);
            if (rc == AEDX_OK) rc = aedx_offsets_check(ip, im, it->offsets, i);
            if (rc != AEDX_OK) return rc;
        }
        if (t->batch_count == 0) {
            int rc = aedx_material_ready(p, mat ? mat : p->def);
            if (rc == AEDX_OK) rc = aedx_offsets_check(p, mat ? mat : p->def, NULL, -1);
            if (rc != AEDX_OK) return rc;
        }
        int src = aedx_streams_check(t, p);
        if (src != AEDX_OK) return src;
    }
    int slot = t->next_frame;
    int rc = aedx_wait_frame(t, slot);
    if (rc != AEDX_OK) return rc;
    AedxFrame* fr = &t->frames[slot];
    rc = aedx_record(t, fr, p, mat, clear);
    if (rc != AEDX_OK) return rc;
    UINT64 v = aedx_execute(t->dev, fr->list);
    if (!v) return AEDX_ERR_DEVICE_LOST;
    fr->value = v;
    fr->submitted = 1;
    fr->seq = ++t->submitted_seq;
    t->rendered = 1;
    t->last_submitted = slot;
    t->next_frame = (slot + 1) % t->frame_count;
    return slot;
}

static int aedx_check_draw(AedxTarget* t, AedxPipeline* p, AedxMaterial* mat) {
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    if (p && p->dev != t->dev) return aedx_fail(AEDX_ERR_ARG, "pipeline belongs to another device");
    if (mat && mat->pipe != p) return aedx_fail(AEDX_ERR_ARG, "material belongs to another pipeline");
    return AEDX_OK;
}

int aedx_submit(AedxTarget* t, AedxPipeline* p, AedxMaterial* mat, float r, float g, float b, float a) {
    aedx_clear_error();
    int rc = aedx_check_draw(t, p, mat);
    if (rc != AEDX_OK) return rc;
    float clear[4] = { r, g, b, a };
    AcquireSRWLockExclusive(&t->dev->lock);
    rc = aedx_submit_locked(t, p, mat, clear);
    ReleaseSRWLockExclusive(&t->dev->lock);
    return rc;
}

int aedx_draw(AedxTarget* t, AedxPipeline* p, AedxMaterial* mat, float r, float g, float b, float a) {
    aedx_clear_error();
    int rc = aedx_check_draw(t, p, mat);
    if (rc != AEDX_OK) return rc;
    float clear[4] = { r, g, b, a };
    AcquireSRWLockExclusive(&t->dev->lock);
    int slot = aedx_submit_locked(t, p, mat, clear);
    rc = slot < 0 ? slot : aedx_wait_frame(t, slot);
    ReleaseSRWLockExclusive(&t->dev->lock);
    return rc;
}

int aedx_wait_all(AedxTarget* t) {
    aedx_clear_error();
    if (!t) return aedx_fail(AEDX_ERR_ARG, "target is null");
    AcquireSRWLockExclusive(&t->dev->lock);
    int rc = aedx_wait_all_locked(t);
    ReleaseSRWLockExclusive(&t->dev->lock);
    return rc;
}

/* The newest frame's readback memory, waited for. NULL with the reason set. */
static const unsigned char* aedx_readable(AedxTarget* t) {
    if (!t->readback_on) {
        aedx_fail(AEDX_ERR_ARG, "readback is off for this target (target_set_readback)");
        return NULL;
    }
    if (t->last_submitted < 0) {
        aedx_fail(AEDX_ERR_ARG, "the target has no frame yet: draw or submit first");
        return NULL;
    }
    AcquireSRWLockExclusive(&t->dev->lock);
    int rc = aedx_wait_frame(t, t->last_submitted);
    ReleaseSRWLockExclusive(&t->dev->lock);
    if (rc != AEDX_OK) return NULL;
    return t->frames[t->last_submitted].readback_ptr;
}

/* Attachment `att`'s readback memory of the newest frame, waited for
 * (#2386). NULL with the reason set. */
static const unsigned char* aedx_readable_at(AedxTarget* t, int att) {
    const unsigned char* base = aedx_readable(t);
    if (!base || att == 0) return base;
    return t->frames[t->last_submitted].xreadback_ptr[att - 1];
}

/* Rows in the readback are RowPitch apart (a multiple of 256), not
 * width * bpp: every reader goes through the footprint. */
static const unsigned char* aedx_px(const AedxTarget* t, const unsigned char* base, int x, int y) {
    return base + t->footprint.Offset + (size_t)y * t->footprint.Footprint.RowPitch + (size_t)x * (size_t)t->bpp;
}

int aedx_read_rgba(AedxTarget* t, void* out, size_t out_len) {
    aedx_clear_error();
    if (!t || !out) return aedx_fail(AEDX_ERR_ARG, "target or destination is null");
    size_t row = (size_t)t->width * (size_t)t->bpp;
    if (out_len < row * (size_t)t->height) {
        return aedx_fail(AEDX_ERR_ARG, "destination holds %zu bytes, the image needs %zu", out_len,
                         row * (size_t)t->height);
    }
    const unsigned char* base = aedx_readable(t);
    if (!base) return AEDX_ERR_ARG;
    for (int y = 0; y < t->height; y++) memcpy((unsigned char*)out + row * (size_t)y, aedx_px(t, base, 0, y), row);
    return AEDX_OK;
}

int aedx_read_rgba8(AedxTarget* t, void* out, size_t out_len) {
    aedx_clear_error();
    if (!t || !out) return aedx_fail(AEDX_ERR_ARG, "target or destination is null");
    size_t need = (size_t)t->width * (size_t)t->height * 4u;
    if (out_len < need) return aedx_fail(AEDX_ERR_ARG, "destination holds %zu bytes, the image needs %zu", out_len, need);
    const unsigned char* base = aedx_readable(t);
    if (!base) return AEDX_ERR_ARG;
    unsigned char* dst = (unsigned char*)out;
    for (int y = 0; y < t->height; y++) {
        for (int x = 0; x < t->width; x++) {
            const unsigned char* px = aedx_px(t, base, x, y);
            for (int c = 0; c < 4; c++) *dst++ = aedx_channel_u8(t, px, c);
        }
    }
    return AEDX_OK;
}

/* ------------------------------------------------------------------------ */
/* Compute                                                                   */
/* ------------------------------------------------------------------------ */

struct AedxCompute {
    AedxDevice*                dev;
    AedxRootLayout             rl;
    ID3D12PipelineState*       pso;
    int                        push_bytes;
    unsigned char              push[AEDX_MAX_PUSH];
    AedxMaterial               args;     /* the resources bound, as a material holds them */
    ID3D12CommandAllocator*    alloc;
    ID3D12GraphicsCommandList* list;
    HANDLE                     event;
    UINT64                     value;
    int                        submitted;
    DWORD                      timeout_ms;
    /* GPU timing (#2198): the dispatch's start and end timestamps. */
    ID3D12QueryHeap*           timer_heap;
    ID3D12Resource*            timer_rb;
    UINT64*                    timer_ptr;
    int                        timing_on;
    int                        timed;
    double                     gpu_ms;
};

void aedx_compute_destroy(AedxCompute* c) {
    if (!c) return;
    AedxDevice* d = c->dev;
    AcquireSRWLockExclusive(&d->lock);
    aedx_idle(d);
    ReleaseSRWLockExclusive(&d->lock);
    for (int i = 0; i < AEDX_MAX_DESC; i++) {
        if (c->args.ub[i].res) {
            ID3D12Resource_Unmap(c->args.ub[i].res, 0, NULL);
            ID3D12Resource_Release(c->args.ub[i].res);
        }
    }
    aedx_free_timer(&c->timer_heap, &c->timer_rb);
    if (c->list) ID3D12GraphicsCommandList_Release(c->list);
    if (c->alloc) ID3D12CommandAllocator_Release(c->alloc);
    if (c->event) CloseHandle(c->event);
    if (c->pso) ID3D12PipelineState_Release(c->pso);
    if (c->rl.root) ID3D12RootSignature_Release(c->rl.root);
    free(c);
}

AedxCompute* aedx_compute_create(AedxDevice* d, const void* cs, size_t len, const AedxBindings* bindings,
                                 int push_bytes) {
    aedx_clear_error();
    if (!d) { aedx_fail(AEDX_ERR_ARG, "device is null"); return NULL; }
    if (push_bytes < 0 || push_bytes > AEDX_MAX_PUSH || (push_bytes % 4)) {
        aedx_fail(AEDX_ERR_ARG, "push constant block must be 0..%d bytes and a multiple of 4 (got %d)",
                  AEDX_MAX_PUSH, push_bytes);
        return NULL;
    }
    ID3DBlob* blob = NULL;
    if (aedx_shader(cs, len, "cs_5_1", &blob) != AEDX_OK) return NULL;
    AedxCompute* c = (AedxCompute*)calloc(1, sizeof(*c));
    if (!c) { ID3D10Blob_Release(blob); aedx_fail(AEDX_ERR_OOM, "out of memory"); return NULL; }
    c->dev = d;
    c->push_bytes = push_bytes;
    c->timeout_ms = 5000;
    c->gpu_ms = -1.0;
    if (aedx_make_root(d, bindings, push_bytes, 1, &c->rl) != AEDX_OK) goto fail;
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd;
    memset(&cd, 0, sizeof(cd));
    cd.pRootSignature = c->rl.root;
    cd.CS.pShaderBytecode = ID3D10Blob_GetBufferPointer(blob);
    cd.CS.BytecodeLength = ID3D10Blob_GetBufferSize(blob);
    HRESULT hr = ID3D12Device_CreateComputePipelineState(d->device, &cd, &IID_ID3D12PipelineState, (void**)&c->pso);
    if (FAILED(hr)) {
        aedx_fail(AEDX_ERR_SHADER, "CreateComputePipelineState failed (0x%08lx): the shader's registers "
                  "must match the bindings", (unsigned long)hr);
        goto fail;
    }
    ID3D10Blob_Release(blob);
    blob = NULL;
    if (aedx_make_list(d, &c->alloc, &c->list) != AEDX_OK) goto fail;
    c->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!c->event) { aedx_fail(AEDX_ERR_OOM, "CreateEvent failed"); goto fail; }
    aedx_drain_debug(d);
    return c;
fail:
    if (blob) ID3D10Blob_Release(blob);
    aedx_compute_destroy(c);
    return NULL;
}

static int aedx_compute_binding(AedxCompute* c, int binding) {
    if (!c) return aedx_fail(AEDX_ERR_ARG, "compute is null");
    if (binding < 0 || binding >= AEDX_MAX_DESC) return aedx_fail(AEDX_ERR_ARG, "binding must be 0..%d", AEDX_MAX_DESC - 1);
    if (!c->rl.kind[binding]) return aedx_fail(AEDX_ERR_ARG, "binding %d was not declared", binding);
    if (c->submitted) return aedx_fail(AEDX_ERR_ARG, "a dispatch is in flight: compute_wait before rebinding");
    return AEDX_OK;
}

int aedx_compute_set_buffer(AedxCompute* c, int binding, AedxBuffer* buf) {
    aedx_clear_error();
    int rc = aedx_compute_binding(c, binding);
    if (rc != AEDX_OK) return rc;
    if (!buf) return aedx_fail(AEDX_ERR_ARG, "buffer is null");
    if (buf->dev != c->dev) return aedx_fail(AEDX_ERR_ARG, "the buffer belongs to another device");
    if (c->rl.kind[binding] == AEDX_BIND_TEXTURE) {
        return aedx_fail(AEDX_ERR_ARG, "binding %d is declared as a texture, not a buffer", binding);
    }
    c->args.buf[binding] = buf;
    c->args.set[binding] = 1;
    return AEDX_OK;
}

/* Binds a storage texture for the pass to write (#2388). */
int aedx_compute_set_storage_texture(AedxCompute* c, int binding, AedxTexture* tex) {
    aedx_clear_error();
    if (!c) return aedx_fail(AEDX_ERR_ARG, "compute is null");
    if (binding < 0 || binding >= AEDX_MAX_DESC) {
        return aedx_fail(AEDX_ERR_ARG, "binding must be 0..%d", AEDX_MAX_DESC - 1);
    }
    if (c->rl.kind[binding] != AEDX_BIND_STORAGE_TEXTURE) {
        return aedx_fail(AEDX_ERR_ARG, "binding %d is not declared as a storage texture", binding);
    }
    if (!tex) return aedx_fail(AEDX_ERR_ARG, "texture is null");
    if (tex->dev != c->dev) return aedx_fail(AEDX_ERR_ARG, "the texture belongs to another device");
    if (!tex->storage) return aedx_fail(AEDX_ERR_ARG, "the texture was not made with texture_create_storage");
    c->args.tex[binding] = tex;
    c->args.set[binding] = 1;
    return AEDX_OK;
}

int aedx_compute_set_texture(AedxCompute* c, int binding, AedxTexture* tex) {
    aedx_clear_error();
    int rc = aedx_compute_binding(c, binding);
    if (rc != AEDX_OK) return rc;
    if (!tex) return aedx_fail(AEDX_ERR_ARG, "texture is null");
    if (c->rl.kind[binding] != AEDX_BIND_TEXTURE) {
        return aedx_fail(AEDX_ERR_ARG, "binding %d is declared as a buffer, not a texture", binding);
    }
    rc = aedx_texture_ready(tex);
    if (rc != AEDX_OK) return rc;
    c->args.tex[binding] = tex;
    c->args.set[binding] = 1;
    return AEDX_OK;
}

int aedx_compute_set_push(AedxCompute* c, const void* data, size_t len) {
    aedx_clear_error();
    if (!c) return aedx_fail(AEDX_ERR_ARG, "compute is null");
    if (len > (size_t)c->push_bytes) return aedx_fail(AEDX_ERR_ARG, "the pipeline declared %d push bytes, got %zu", c->push_bytes, len);
    if (len > 0 && !data) return aedx_fail(AEDX_ERR_ARG, "push data is null");
    if (len > 0) memcpy(c->push, data, len);
    return AEDX_OK;
}

int aedx_compute_set_timeout_ms(AedxCompute* c, int ms) {
    aedx_clear_error();
    if (!c) return aedx_fail(AEDX_ERR_ARG, "compute is null");
    if (ms <= 0) return aedx_fail(AEDX_ERR_ARG, "timeout must be positive (got %d)", ms);
    c->timeout_ms = (DWORD)ms;
    return AEDX_OK;
}

static int aedx_compute_wait_locked(AedxCompute* c) {
    if (!c->submitted) return AEDX_OK;
    int rc = aedx_wait_value(c->dev, c->value, c->event, c->timeout_ms);
    if (rc == AEDX_OK) {
        c->submitted = 0;
        if (c->timed && c->timer_ptr) c->gpu_ms = aedx_timer_ms(c->dev, c->timer_ptr);
    }
    return rc;
}

int aedx_compute_set_timing(AedxCompute* c, int on) {
    aedx_clear_error();
    if (!c) return aedx_fail(AEDX_ERR_ARG, "compute is null");
    on = on ? 1 : 0;
    AedxDevice* d = c->dev;
    AcquireSRWLockExclusive(&d->lock);
    int rc = AEDX_OK;
    if (on && !c->timer_heap) rc = aedx_make_timer(d, 2, &c->timer_heap, &c->timer_rb, &c->timer_ptr);
    if (rc == AEDX_OK) {
        c->timing_on = on;
        c->gpu_ms = -1.0;
    }
    ReleaseSRWLockExclusive(&d->lock);
    return rc;
}

double aedx_compute_gpu_ms(const AedxCompute* c) { return (c && c->timing_on) ? c->gpu_ms : -1.0; }

int aedx_dispatch_async(AedxCompute* c, int gx, int gy, int gz) {
    aedx_clear_error();
    if (!c) return aedx_fail(AEDX_ERR_ARG, "compute is null");
    if (gx <= 0 || gy <= 0 || gz <= 0) {
        return aedx_fail(AEDX_ERR_ARG, "work group counts must be positive, got %d x %d x %d", gx, gy, gz);
    }
    const int limit = D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
    if (gx > limit || gy > limit || gz > limit) {
        return aedx_fail(AEDX_ERR_UNSUPPORTED, "%d x %d x %d work groups exceeds the device limit of %d a dimension",
                         gx, gy, gz, limit);
    }
    for (int i = 0; i < AEDX_MAX_DESC; i++) {
        if (c->rl.kind[i] && !c->args.set[i]) return aedx_fail(AEDX_ERR_ARG, "binding %d was declared but never set", i);
    }
    AedxDevice* d = c->dev;
    AcquireSRWLockExclusive(&d->lock);
    int rc = aedx_compute_wait_locked(c);
    if (rc != AEDX_OK) { ReleaseSRWLockExclusive(&d->lock); return rc; }
    HRESULT hr = ID3D12CommandAllocator_Reset(c->alloc);
    if (SUCCEEDED(hr)) hr = ID3D12GraphicsCommandList_Reset(c->list, c->alloc, c->pso);
    if (FAILED(hr)) {
        ReleaseSRWLockExclusive(&d->lock);
        return aedx_fail(aedx_hr_status(hr), "cannot reset the command list (0x%08lx)", (unsigned long)hr);
    }
    ID3D12GraphicsCommandList* l = c->list;
    c->timed = c->timing_on && c->timer_heap;
    if (c->timed) ID3D12GraphicsCommandList_EndQuery(l, c->timer_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    ID3D12GraphicsCommandList_SetComputeRootSignature(l, c->rl.root);
    ID3D12DescriptorHeap* heaps[2] = { d->srv.heap, d->samplers.heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(l, 2, heaps);
    if (c->rl.push_words > 0) {
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(l, 0, c->rl.push_words, c->push, 0);
    }
    aedx_bind_material(d, l, &c->rl, &c->args, 1, NULL);
    /* Storage textures rest in the shader-resource state; the pass writes
     * them in UNORDERED_ACCESS (#2388). */
    for (int i = 0; i < AEDX_MAX_DESC; i++) {
        if (c->rl.kind[i] != AEDX_BIND_STORAGE_TEXTURE) continue;
        aedx_barrier(l, c->args.tex[i]->res,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    ID3D12GraphicsCommandList_Dispatch(l, (UINT)gx, (UINT)gy, (UINT)gz);
    /* Writes made visible to whatever reads the buffers next: a later
     * dispatch, a draw, or (after the fence) the CPU. */
    D3D12_RESOURCE_BARRIER uav;
    memset(&uav, 0, sizeof(uav));
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = NULL;
    ID3D12GraphicsCommandList_ResourceBarrier(l, 1, &uav);
    for (int i = 0; i < AEDX_MAX_DESC; i++) {
        if (c->rl.kind[i] != AEDX_BIND_STORAGE_TEXTURE) continue;
        aedx_barrier(l, c->args.tex[i]->res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (c->timed) {
        ID3D12GraphicsCommandList_EndQuery(l, c->timer_heap, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        ID3D12GraphicsCommandList_ResolveQueryData(l, c->timer_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                                                   c->timer_rb, 0);
    }
    hr = ID3D12GraphicsCommandList_Close(l);
    if (FAILED(hr)) {
        aedx_drain_debug(d);
        ReleaseSRWLockExclusive(&d->lock);
        return aedx_fail(AEDX_ERR_ARG, "the dispatch's commands were invalid (0x%08lx)", (unsigned long)hr);
    }
    UINT64 v = aedx_execute(d, l);
    if (v) { c->value = v; c->submitted = 1; }
    ReleaseSRWLockExclusive(&d->lock);
    return v ? AEDX_OK : AEDX_ERR_DEVICE_LOST;
}

int aedx_compute_wait(AedxCompute* c) {
    aedx_clear_error();
    if (!c) return aedx_fail(AEDX_ERR_ARG, "compute is null");
    AcquireSRWLockExclusive(&c->dev->lock);
    int rc = aedx_compute_wait_locked(c);
    ReleaseSRWLockExclusive(&c->dev->lock);
    return rc;
}

int aedx_dispatch(AedxCompute* c, int gx, int gy, int gz) {
    int rc = aedx_dispatch_async(c, gx, gy, gz);
    if (rc != AEDX_OK) return rc;
    return aedx_compute_wait(c);
}

/* ------------------------------------------------------------------------ */
/* Presentation                                                              */
/* ------------------------------------------------------------------------ */

/* The present pass: a full-screen triangle sampling the target. Direct3D 12
 * has no blit, so this is how a frame is scaled to the window and, through
 * an sRGB view of the back buffer, encoded for display. */
static const char k_present_hlsl[] =
    "Texture2D src : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "    VSOut o;\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "    o.uv = uv;\n"
    "    return o;\n"
    "}\n"
    "float4 ps_main(VSOut i) : SV_Target { return src.Sample(smp, i.uv); }\n";

#define AEDX_BACKBUFFER_FORMAT DXGI_FORMAT_B8G8R8A8_UNORM
#define AEDX_BACKBUFFERS 3
#define AEDX_PRESENT_FRAMES 2

/* THE DEVICE LOCK MUST BE HELD. */
static int aedx_present_pass(AedxDevice* d) {
    if (d->present_pso[0] && d->present_pso[1]) return AEDX_OK;
    if (!g_dx.compile) return aedx_fail(AEDX_ERR_SHADER, "presenting needs d3dcompiler_47.dll, which is missing");
    ID3DBlob* vs = NULL;
    ID3DBlob* ps = NULL;
    ID3DBlob* errors = NULL;
    HRESULT hr = g_dx.compile(k_present_hlsl, sizeof(k_present_hlsl) - 1, "present", NULL, NULL, "vs_main",
                              "vs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors);
    if (SUCCEEDED(hr)) {
        if (errors) { ID3D10Blob_Release(errors); errors = NULL; }
        hr = g_dx.compile(k_present_hlsl, sizeof(k_present_hlsl) - 1, "present", NULL, NULL, "ps_main",
                          "ps_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors);
    }
    if (FAILED(hr)) {
        aedx_fail(AEDX_ERR_SHADER, "the present pass did not compile: %s",
                  errors ? (const char*)ID3D10Blob_GetBufferPointer(errors) : "no message");
        if (errors) ID3D10Blob_Release(errors);
        if (vs) ID3D10Blob_Release(vs);
        return AEDX_ERR_SHADER;
    }
    if (errors) ID3D10Blob_Release(errors);

    int rc = AEDX_OK;
    if (!d->present_root) {
        D3D12_DESCRIPTOR_RANGE range;
        memset(&range, 0, sizeof(range));
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER param;
        memset(&param, 0, sizeof(param));
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges = &range;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC smp;
        memset(&smp, 0, sizeof(smp));
        smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp.MaxLOD = D3D12_FLOAT32_MAX;
        smp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rs;
        memset(&rs, 0, sizeof(rs));
        rs.NumParameters = 1;
        rs.pParameters = &param;
        rs.NumStaticSamplers = 1;
        rs.pStaticSamplers = &smp;
        ID3DBlob* blob = NULL;
        hr = g_dx.serialize_root(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, NULL);
        if (SUCCEEDED(hr)) {
            hr = ID3D12Device_CreateRootSignature(d->device, 0, ID3D10Blob_GetBufferPointer(blob),
                                                  ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature,
                                                  (void**)&d->present_root);
            ID3D10Blob_Release(blob);
        }
        if (FAILED(hr)) rc = aedx_fail(AEDX_ERR_OOM, "the present root signature failed (0x%08lx)", (unsigned long)hr);
    }
    for (int s = 0; s < 2 && rc == AEDX_OK; s++) {
        if (d->present_pso[s]) continue;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gd;
        memset(&gd, 0, sizeof(gd));
        gd.pRootSignature = d->present_root;
        gd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
        gd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
        gd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps);
        gd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
        gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        gd.SampleMask = UINT_MAX;
        gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        gd.RasterizerState.DepthClipEnable = TRUE;
        gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gd.NumRenderTargets = 1;
        gd.RTVFormats[0] = s ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
        gd.SampleDesc.Count = 1;
        hr = ID3D12Device_CreateGraphicsPipelineState(d->device, &gd, &IID_ID3D12PipelineState,
                                                      (void**)&d->present_pso[s]);
        if (FAILED(hr)) rc = aedx_fail(AEDX_ERR_OOM, "the present pipeline failed (0x%08lx)", (unsigned long)hr);
    }
    ID3D10Blob_Release(vs);
    ID3D10Blob_Release(ps);
    return rc;
}

typedef struct {
    ID3D12CommandAllocator*    alloc;
    ID3D12GraphicsCommandList* list;
    HANDLE                     event;
    UINT64                     value;
    int                        submitted;
} AedxPresentFrame;

struct AedxSwapchain {
    AedxDevice*           dev;
    HWND                  hwnd;
    IDXGISwapChain3*      sc;
    UINT                  buf_w, buf_h;   /* the back buffers' size */
    UINT                  width, height;  /* reported: 0 x 0 while the window has no area */
    UINT                  flags;
    ID3D12Resource*       buffers[AEDX_BACKBUFFERS];
    ID3D12DescriptorHeap* rtv_heap;       /* two views a buffer: UNORM, then sRGB */
    int                   vsync;
    AedxPresentFrame      frames[AEDX_PRESENT_FRAMES];
    int                   frame;
    long long             presented;
    DWORD                 timeout_ms;
};

static void aedx_sc_release_buffers(AedxSwapchain* s) {
    for (int i = 0; i < AEDX_BACKBUFFERS; i++) {
        if (s->buffers[i]) { ID3D12Resource_Release(s->buffers[i]); s->buffers[i] = NULL; }
    }
}

static int aedx_sc_views(AedxSwapchain* s) {
    AedxDevice* d = s->dev;
    D3D12_CPU_DESCRIPTOR_HANDLE h = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(s->rtv_heap);
    for (UINT i = 0; i < AEDX_BACKBUFFERS; i++) {
        HRESULT hr = IDXGISwapChain3_GetBuffer(s->sc, i, &IID_ID3D12Resource, (void**)&s->buffers[i]);
        if (FAILED(hr)) return aedx_fail(aedx_hr_status(hr), "GetBuffer failed (0x%08lx)", (unsigned long)hr);
        for (int srgb = 0; srgb < 2; srgb++) {
            D3D12_RENDER_TARGET_VIEW_DESC rv;
            memset(&rv, 0, sizeof(rv));
            rv.Format = srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
            rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE at = h;
            at.ptr += (SIZE_T)(i * 2 + (UINT)srgb) * d->rtv_inc;
            ID3D12Device_CreateRenderTargetView(d->device, s->buffers[i], &rv, at);
        }
    }
    return AEDX_OK;
}

static void aedx_client_size(HWND hwnd, UINT* w, UINT* h) {
    RECT r;
    if (!GetClientRect(hwnd, &r)) { *w = *h = 0; return; }
    *w = (UINT)(r.right - r.left);
    *h = (UINT)(r.bottom - r.top);
}

/* Follows the window: resizes the back buffers when its client area
 * changed, and reports 0 x 0 while it has none. THE DEVICE LOCK MUST BE
 * HELD. */
static int aedx_sc_follow(AedxSwapchain* s) {
    UINT w, h;
    aedx_client_size(s->hwnd, &w, &h);
    if (w == 0 || h == 0) {
        s->width = s->height = 0;
        return AEDX_OK;
    }
    if (w != s->buf_w || h != s->buf_h) {
        int rc = aedx_idle(s->dev);
        if (rc != AEDX_OK) return rc;
        for (int i = 0; i < AEDX_PRESENT_FRAMES; i++) s->frames[i].submitted = 0;
        aedx_sc_release_buffers(s);
        HRESULT hr = IDXGISwapChain3_ResizeBuffers(s->sc, AEDX_BACKBUFFERS, w, h, AEDX_BACKBUFFER_FORMAT, s->flags);
        if (FAILED(hr)) return aedx_fail(aedx_hr_status(hr), "ResizeBuffers failed (0x%08lx)", (unsigned long)hr);
        rc = aedx_sc_views(s);
        if (rc != AEDX_OK) return rc;
        s->buf_w = w;
        s->buf_h = h;
    }
    s->width = w;
    s->height = h;
    return AEDX_OK;
}

void aedx_swapchain_destroy(AedxSwapchain* s) {
    if (!s) return;
    AedxDevice* d = s->dev;
    AcquireSRWLockExclusive(&d->lock);
    aedx_idle(d);
    ReleaseSRWLockExclusive(&d->lock);
    for (int i = 0; i < AEDX_PRESENT_FRAMES; i++) {
        AedxPresentFrame* f = &s->frames[i];
        if (f->list) ID3D12GraphicsCommandList_Release(f->list);
        if (f->alloc) ID3D12CommandAllocator_Release(f->alloc);
        if (f->event) CloseHandle(f->event);
    }
    aedx_sc_release_buffers(s);
    if (s->rtv_heap) ID3D12DescriptorHeap_Release(s->rtv_heap);
    if (s->sc) IDXGISwapChain3_Release(s->sc);
    free(s);
}

AedxSwapchain* aedx_swapchain_create(AedxDevice* d, int kind, void* display, void* window, int width, int height) {
    aedx_clear_error();
    (void)display;
    if (!d) { aedx_fail(AEDX_ERR_ARG, "device is null"); return NULL; }
    if (width < 0 || height < 0) { aedx_fail(AEDX_ERR_ARG, "size must not be negative, got %dx%d", width, height); return NULL; }
    if (kind != AEDX_WINDOW_WIN32) {
        aedx_fail(kind >= 1 && kind <= 5 ? AEDX_ERR_UNSUPPORTED : AEDX_ERR_ARG,
                  "Direct3D 12 presents to a Win32 window (kind 1), not kind %d", kind);
        return NULL;
    }
    if (!window || !IsWindow((HWND)window)) { aedx_fail(AEDX_ERR_ARG, "window handle is not a window"); return NULL; }
    AedxSwapchain* s = (AedxSwapchain*)calloc(1, sizeof(*s));
    if (!s) { aedx_fail(AEDX_ERR_OOM, "out of memory"); return NULL; }
    s->dev = d;
    s->hwnd = (HWND)window;
    s->vsync = 1;
    s->timeout_ms = 5000;
    s->flags = d->allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    UINT w, h;
    aedx_client_size(s->hwnd, &w, &h);
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof(sd));
    sd.Width = w;
    sd.Height = h;
    sd.Format = AEDX_BACKBUFFER_FORMAT;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = AEDX_BACKBUFFERS;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    sd.Flags = s->flags;
    IDXGISwapChain1* sc1 = NULL;
    HRESULT hr = IDXGIFactory4_CreateSwapChainForHwnd(d->factory, (IUnknown*)d->queue, s->hwnd, &sd, NULL, NULL, &sc1);
    if (FAILED(hr)) { aedx_fail(aedx_hr_status(hr), "CreateSwapChainForHwnd failed (0x%08lx)", (unsigned long)hr); goto fail; }
    hr = IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void**)&s->sc);
    IDXGISwapChain1_Release(sc1);
    if (FAILED(hr)) { aedx_fail(AEDX_ERR_UNSUPPORTED, "the swapchain has no IDXGISwapChain3 (0x%08lx)", (unsigned long)hr); goto fail; }
    /* The window is the caller's: DXGI must not take over Alt+Enter. */
    IDXGIFactory4_MakeWindowAssociation(d->factory, s->hwnd, DXGI_MWA_NO_ALT_ENTER);
    DXGI_SWAP_CHAIN_DESC1 got;
    IDXGISwapChain3_GetDesc1(s->sc, &got);
    s->buf_w = got.Width;
    s->buf_h = got.Height;

    D3D12_DESCRIPTOR_HEAP_DESC hd;
    memset(&hd, 0, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = AEDX_BACKBUFFERS * 2;
    hr = ID3D12Device_CreateDescriptorHeap(d->device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s->rtv_heap);
    if (FAILED(hr)) { aedx_fail(AEDX_ERR_OOM, "CreateDescriptorHeap (RTV) failed (0x%08lx)", (unsigned long)hr); goto fail; }
    if (aedx_sc_views(s) != AEDX_OK) goto fail;
    for (int i = 0; i < AEDX_PRESENT_FRAMES; i++) {
        if (aedx_make_list(d, &s->frames[i].alloc, &s->frames[i].list) != AEDX_OK) goto fail;
        s->frames[i].event = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!s->frames[i].event) { aedx_fail(AEDX_ERR_OOM, "CreateEvent failed"); goto fail; }
    }
    AcquireSRWLockExclusive(&d->lock);
    int rc = aedx_present_pass(d);
    if (rc == AEDX_OK) rc = aedx_sc_follow(s);
    ReleaseSRWLockExclusive(&d->lock);
    if (rc != AEDX_OK) goto fail;
    aedx_drain_debug(d);
    return s;
fail: {
        char reason[sizeof(g_err)];
        snprintf(reason, sizeof(reason), "%s", g_err);
        aedx_swapchain_destroy(s);
        snprintf(g_err, sizeof(g_err), "%s", reason);
        return NULL;
    }
}

int aedx_swapchain_resize(AedxSwapchain* s, int width, int height) {
    aedx_clear_error();
    if (!s) return aedx_fail(AEDX_ERR_ARG, "swapchain is null");
    if (width < 0 || height < 0) return aedx_fail(AEDX_ERR_ARG, "size must not be negative, got %dx%d", width, height);
    /* A Win32 swapchain takes the window's own size; the numbers the caller
     * passes are what the toolkit reported, and the window agrees. */
    AcquireSRWLockExclusive(&s->dev->lock);
    int rc = aedx_sc_follow(s);
    ReleaseSRWLockExclusive(&s->dev->lock);
    return rc;
}

int aedx_swapchain_set_vsync(AedxSwapchain* s, int on) {
    aedx_clear_error();
    if (!s) return aedx_fail(AEDX_ERR_ARG, "swapchain is null");
    s->vsync = on ? 1 : 0;
    return AEDX_OK;
}

int aedx_swapchain_width(const AedxSwapchain* s)  { return s ? (int)s->width : 0; }
int aedx_swapchain_height(const AedxSwapchain* s) { return s ? (int)s->height : 0; }
int aedx_swapchain_format(const AedxSwapchain* s) { return s ? (int)AEDX_BACKBUFFER_FORMAT : 0; }
long long aedx_swapchain_presented(const AedxSwapchain* s) { return s ? s->presented : 0; }

int aedx_present(AedxSwapchain* s, AedxTarget* t) {
    aedx_clear_error();
    if (!s || !t) return aedx_fail(AEDX_ERR_ARG, "swapchain or target is null");
    if (s->dev != t->dev) return aedx_fail(AEDX_ERR_ARG, "the target belongs to another device");
    if (t->layered) return aedx_fail(AEDX_ERR_ARG, "a layered target is drawn and sampled, not presented (#2399)");
    if (!t->rendered) return aedx_fail(AEDX_ERR_ARG, "the target has no frame yet: draw or submit before presenting");
    AedxDevice* d = s->dev;
    AcquireSRWLockExclusive(&d->lock);
    int rc = aedx_sc_follow(s);
    if (rc != AEDX_OK || s->width == 0) { ReleaseSRWLockExclusive(&d->lock); return rc; }

    AedxPresentFrame* f = &s->frames[s->frame];
    if (f->submitted) {
        rc = aedx_wait_value(d, f->value, f->event, s->timeout_ms);
        if (rc != AEDX_OK) { ReleaseSRWLockExclusive(&d->lock); return rc; }
        f->submitted = 0;
    }
    HRESULT hr = ID3D12CommandAllocator_Reset(f->alloc);
    if (SUCCEEDED(hr)) hr = ID3D12GraphicsCommandList_Reset(f->list, f->alloc, NULL);
    if (FAILED(hr)) {
        ReleaseSRWLockExclusive(&d->lock);
        return aedx_fail(aedx_hr_status(hr), "cannot reset the command list (0x%08lx)", (unsigned long)hr);
    }
    ID3D12GraphicsCommandList* l = f->list;
    UINT idx = IDXGISwapChain3_GetCurrentBackBufferIndex(s->sc);
    int srgb = aedx_format_srgb_display(t->format);
    aedx_barrier(l, t->color, t->color_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    t->color_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    aedx_barrier(l, s->buffers[idx], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(s->rtv_heap);
    rtv.ptr += (SIZE_T)(idx * 2 + (UINT)srgb) * d->rtv_inc;
    ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, NULL);
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)s->width, (float)s->height, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)s->width, (LONG)s->height };
    ID3D12GraphicsCommandList_RSSetViewports(l, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(l, d->present_root);
    ID3D12GraphicsCommandList_SetPipelineState(l, d->present_pso[srgb]);
    ID3D12DescriptorHeap* heaps[1] = { d->srv.heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(l, 1, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(l, 0, aedx_slot_gpu(&d->srv, t->srv_slot));
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(l, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(l, 3, 1, 0, 0);
    aedx_barrier(l, s->buffers[idx], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    hr = ID3D12GraphicsCommandList_Close(l);
    UINT64 v = SUCCEEDED(hr) ? aedx_execute(d, l) : 0;
    if (!v) {
        ReleaseSRWLockExclusive(&d->lock);
        return FAILED(hr) ? aedx_fail(AEDX_ERR_ARG, "the present commands were invalid (0x%08lx)", (unsigned long)hr)
                          : AEDX_ERR_DEVICE_LOST;
    }
    f->value = v;
    f->submitted = 1;
    s->frame = (s->frame + 1) % AEDX_PRESENT_FRAMES;
    UINT flags = (!s->vsync && (s->flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    hr = IDXGISwapChain3_Present(s->sc, s->vsync ? 1 : 0, flags);
    aedx_drain_debug(d);
    ReleaseSRWLockExclusive(&d->lock);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG) {
        return aedx_fail(AEDX_ERR_DEVICE_LOST, "Present: the device was removed (0x%08lx)", (unsigned long)hr);
    }
    if (FAILED(hr)) return aedx_fail(AEDX_ERR_UNSUPPORTED, "Present failed (0x%08lx)", (unsigned long)hr);
    s->presented++;
    return AEDX_OK;
}

/* ------------------------------------------------------------------------ */
/* Readers and staging that need the objects' insides                        */
/* ------------------------------------------------------------------------ */

/* Packed 0xRRGGBBAA as a non-negative 64-bit value, or -1: 64 bits so that
 * opaque white is distinct from the failure. */
int64_t aedx_ae_pixel(void* tp, int x, int y) {
    AedxTarget* t = (AedxTarget*)tp;
    aedx_clear_error();
    if (!t) { aedx_fail(AEDX_ERR_ARG, "target is null"); return -1; }
    if (x < 0 || y < 0 || x >= t->width || y >= t->height) {
        aedx_fail(AEDX_ERR_ARG, "pixel %d,%d is outside %dx%d", x, y, t->width, t->height);
        return -1;
    }
    const unsigned char* base = aedx_readable(t);
    if (!base) return -1;
    const unsigned char* px = aedx_px(t, base, x, y);
    return (int64_t)(((uint32_t)aedx_channel_u8(t, px, 0) << 24) | ((uint32_t)aedx_channel_u8(t, px, 1) << 16) |
                     ((uint32_t)aedx_channel_u8(t, px, 2) << 8) | (uint32_t)aedx_channel_u8(t, px, 3));
}

double aedx_ae_pixel_value(void* tp, int x, int y, int channel) {
    AedxTarget* t = (AedxTarget*)tp;
    aedx_clear_error();
    if (!t) { aedx_fail(AEDX_ERR_ARG, "target is null"); return (double)NAN; }
    if (x < 0 || y < 0 || x >= t->width || y >= t->height) {
        aedx_fail(AEDX_ERR_ARG, "pixel %d,%d is outside %dx%d", x, y, t->width, t->height);
        return (double)NAN;
    }
    if (channel < 0 || channel > 3) { aedx_fail(AEDX_ERR_ARG, "channel %d is not 0..3", channel); return (double)NAN; }
    const unsigned char* base = aedx_readable(t);
    if (!base) return (double)NAN;
    return (double)aedx_channel_value(t, aedx_px(t, base, x, y), channel);
}

/* Channel `c` of a pixel in format `f`, as aedx_channel_value reads a
 * target's own format (#2386). */
static float aedx_format_channel(DXGI_FORMAT f, const unsigned char* px, int c) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: {
            uint16_t h;
            memcpy(&h, px + c * 2, sizeof(h));
            return aedx_half_to_float(h);
        }
        case DXGI_FORMAT_R32G32B32A32_FLOAT: {
            float v;
            memcpy(&v, px + c * 4, sizeof(v));
            return v;
        }
        default:
            return (float)px[c] / 255.0f;
    }
}

/* One channel of one pixel of colour attachment `att` (#2386). */
double aedx_ae_pixel_value_at(void* tp, int att, int x, int y, int channel) {
    AedxTarget* t = (AedxTarget*)tp;
    aedx_clear_error();
    if (!t) { aedx_fail(AEDX_ERR_ARG, "target is null"); return (double)NAN; }
    if (att < 0 || att > t->extra) {
        aedx_fail(AEDX_ERR_ARG, "the target has colour attachments 0..%d, not %d", t->extra, att);
        return (double)NAN;
    }
    if (att == 0) return aedx_ae_pixel_value(tp, x, y, channel);
    if (x < 0 || y < 0 || x >= t->width || y >= t->height) {
        aedx_fail(AEDX_ERR_ARG, "pixel %d,%d is outside %dx%d", x, y, t->width, t->height);
        return (double)NAN;
    }
    if (channel < 0 || channel > 3) { aedx_fail(AEDX_ERR_ARG, "channel %d is not 0..3", channel); return (double)NAN; }
    const unsigned char* base = aedx_readable_at(t, att);
    if (!base) return (double)NAN;
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp = &t->xfootprint[att - 1];
    const unsigned char* px = base + fp->Offset + (size_t)y * fp->Footprint.RowPitch +
                              (size_t)x * (size_t)t->xbpp[att - 1];
    return (double)aedx_format_channel(t->xformat[att - 1], px, channel);
}

int aedx_ae_save_ppm(void* tp, const char* path) {
    AedxTarget* t = (AedxTarget*)tp;
    aedx_clear_error();
    if (!t || !path) return aedx_fail(AEDX_ERR_ARG, "target or path is null");
    const unsigned char* base = aedx_readable(t);
    if (!base) return AEDX_ERR_ARG;
    FILE* f = fopen(path, "wb");
    if (!f) return aedx_fail(AEDX_ERR_ARG, "cannot open %s for writing", path);
    int ok = fprintf(f, "P6\n%d %d\n255\n", t->width, t->height) > 0;
    for (int y = 0; ok && y < t->height; y++) {
        for (int x = 0; ok && x < t->width; x++) {
            const unsigned char* px = aedx_px(t, base, x, y);
            unsigned char rgb[3] = { aedx_channel_u8(t, px, 0), aedx_channel_u8(t, px, 1), aedx_channel_u8(t, px, 2) };
            ok = fwrite(rgb, 1, 3, f) == 3;
        }
    }
    if (fclose(f) != 0) ok = 0;
    return ok ? AEDX_OK : aedx_fail(AEDX_ERR_ARG, "short write to %s", path);
}

/* A uniform of `count` floats, zeroed, that uniform_float then fills: a
 * caller writes a colour or a matrix as numbers, not packed bytes. */
static int aedx_floats(AedxMaterial* m, int binding, int count) {
    if (count <= 0) return aedx_fail(AEDX_ERR_ARG, "uniform float count must be positive");
    float* zero = (float*)calloc((size_t)count, sizeof(float));
    if (!zero) return aedx_fail(AEDX_ERR_OOM, "out of memory");
    int rc = aedx_material_set_uniform(m, binding, zero, (size_t)count * sizeof(float));
    free(zero);
    return rc;
}

static int aedx_float(AedxMaterial* m, int binding, int index, double value, const char* first) {
    if (!m) return aedx_fail(AEDX_ERR_ARG, "material is null");
    if (binding < 0 || binding >= AEDX_MAX_DESC) return aedx_fail(AEDX_ERR_ARG, "binding must be 0..%d", AEDX_MAX_DESC - 1);
    if (!m->ub[binding].ptr || m->buf[binding]) return aedx_fail(AEDX_ERR_ARG, "call %s for binding %d first", first, binding);
    int n = (int)(m->ub[binding].size / sizeof(float));
    if (index < 0 || index >= n) return aedx_fail(AEDX_ERR_ARG, "uniform float %d is outside 0..%d", index, n - 1);
    float f = (float)value;
    memcpy(m->ub[binding].ptr + (size_t)index * sizeof(float), &f, sizeof(f));
    return AEDX_OK;
}

static AedxMaterial* aedx_default_material(AedxPipeline* p) {
    if (!p) { aedx_fail(AEDX_ERR_ARG, "pipeline is null"); return NULL; }
    if (!p->def) aedx_fail(AEDX_ERR_ARG, "pipeline was created without bindings, so it has no materials");
    return p->def;
}

int aedx_ae_uniform_floats(void* p, int binding, int count) {
    aedx_clear_error();
    AedxMaterial* m = aedx_default_material((AedxPipeline*)p);
    return m ? aedx_floats(m, binding, count) : AEDX_ERR_ARG;
}
int aedx_ae_uniform_float(void* p, int binding, int index, double value) {
    aedx_clear_error();
    AedxMaterial* m = aedx_default_material((AedxPipeline*)p);
    return m ? aedx_float(m, binding, index, value, "uniform_floats") : AEDX_ERR_ARG;
}
int aedx_ae_material_floats(void* m, int binding, int count) {
    aedx_clear_error();
    if (!m) return aedx_fail(AEDX_ERR_ARG, "material is null");
    return aedx_floats((AedxMaterial*)m, binding, count);
}
int aedx_ae_material_float(void* m, int binding, int index, double value) {
    aedx_clear_error();
    return aedx_float((AedxMaterial*)m, binding, index, value, "material_floats");
}
int aedx_ae_set_uniform(void* p, int binding, const void* data, int len) {
    aedx_clear_error();
    if (len < 0) return aedx_fail(AEDX_ERR_ARG, "negative uniform length");
    AedxMaterial* m = aedx_default_material((AedxPipeline*)p);
    return m ? aedx_material_set_uniform(m, binding, data, (size_t)len) : AEDX_ERR_ARG;
}
int aedx_ae_set_texture(void* p, int binding, void* tex) {
    aedx_clear_error();
    AedxMaterial* m = aedx_default_material((AedxPipeline*)p);
    return m ? aedx_material_set_texture(m, binding, (AedxTexture*)tex) : AEDX_ERR_ARG;
}
int aedx_ae_set_target(void* p, int binding, void* t, int depth) {
    aedx_clear_error();
    AedxMaterial* m = aedx_default_material((AedxPipeline*)p);
    return m ? aedx_material_set_target(m, binding, (AedxTarget*)t, depth) : AEDX_ERR_ARG;
}
int aedx_ae_set_buffer(void* p, int binding, void* buf) {
    aedx_clear_error();
    AedxMaterial* m = aedx_default_material((AedxPipeline*)p);
    return m ? aedx_material_set_buffer(m, binding, (AedxBuffer*)buf) : AEDX_ERR_ARG;
}

static int aedx_compute_push_word(AedxCompute* c, int index, const void* word) {
    aedx_clear_error();
    if (!c) return aedx_fail(AEDX_ERR_ARG, "compute is null");
    int n = c->push_bytes / 4;
    if (index < 0 || index >= n) return aedx_fail(AEDX_ERR_ARG, "push slot %d is outside 0..%d", index, n - 1);
    memcpy(c->push + (size_t)index * 4u, word, 4);
    return AEDX_OK;
}
int aedx_ae_compute_push_float(void* c, int index, double value) {
    float f = (float)value;
    return aedx_compute_push_word((AedxCompute*)c, index, &f);
}
int aedx_ae_compute_push_int(void* c, int index, int value) {
    int32_t v = (int32_t)value;
    return aedx_compute_push_word((AedxCompute*)c, index, &v);
}

#else /* not _WIN32: Direct3D 12 exists only on Windows */

static int aedx_no(void) {
    return aedx_fail(AEDX_ERR_NO_LOADER, "Direct3D 12 is available on Windows only");
}

struct AedxDevice { int unused; };

int         aedx_available(void) { aedx_no(); return 0; }
const char* aedx_device_name(void) { return ""; }
AedxDevice* aedx_device_create(void) { aedx_no(); return NULL; }
void        aedx_device_destroy(AedxDevice* d) { (void)d; }
int         aedx_device_is_warp(const AedxDevice* d) { (void)d; return 0; }
int         aedx_debug_message_count(const AedxDevice* d) { (void)d; return 0; }
AedxTarget* aedx_target_create(AedxDevice* d, int w, int h) { (void)d; (void)w; (void)h; aedx_no(); return NULL; }
AedxTarget* aedx_target_create_ex(AedxDevice* d, int w, int h, int dp, int s) {
    (void)d; (void)w; (void)h; (void)dp; (void)s; aedx_no(); return NULL;
}
AedxTarget* aedx_target_create_format(AedxDevice* d, int w, int h, int f, int dp, int s) {
    (void)d; (void)w; (void)h; (void)f; (void)dp; (void)s; aedx_no(); return NULL;
}
void   aedx_target_destroy(AedxTarget* t) { (void)t; }
int    aedx_target_width(const AedxTarget* t) { (void)t; return 0; }
int    aedx_target_height(const AedxTarget* t) { (void)t; return 0; }
int    aedx_target_format(const AedxTarget* t) { (void)t; return 0; }
int    aedx_target_bytes_per_pixel(const AedxTarget* t) { (void)t; return 0; }
int    aedx_target_has_depth(const AedxTarget* t) { (void)t; return 0; }
int    aedx_target_samples(const AedxTarget* t) { (void)t; return 0; }
int    aedx_target_resize(AedxTarget* t, int w, int h) { (void)t; (void)w; (void)h; return aedx_no(); }
int    aedx_target_set_readback(AedxTarget* t, int on) { (void)t; (void)on; return aedx_no(); }
int    aedx_target_readback(const AedxTarget* t) { (void)t; return 0; }
int    aedx_target_set_frames(AedxTarget* t, int n) { (void)t; (void)n; return aedx_no(); }
int    aedx_target_frames(const AedxTarget* t) { (void)t; return 0; }
int    aedx_target_set_timeout_ms(AedxTarget* t, int ms) { (void)t; (void)ms; return aedx_no(); }
size_t aedx_rgba_size(const AedxTarget* t) { (void)t; return 0; }
AedxLayout* aedx_layout_create(void) { aedx_no(); return NULL; }
void   aedx_layout_destroy(AedxLayout* l) { (void)l; }
int    aedx_layout_binding(AedxLayout* l, int b, int s, int i) { (void)l; (void)b; (void)s; (void)i; return aedx_no(); }
int    aedx_layout_attr(AedxLayout* l, int lo, int b, int f, int o) { (void)l; (void)lo; (void)b; (void)f; (void)o; return aedx_no(); }
AedxBindings* aedx_bindings_create(void) { aedx_no(); return NULL; }
void   aedx_bindings_destroy(AedxBindings* b) { (void)b; }
int    aedx_bindings_uniform(AedxBindings* b, int n) { (void)b; (void)n; return aedx_no(); }
int    aedx_bindings_texture(AedxBindings* b, int n) { (void)b; (void)n; return aedx_no(); }
int    aedx_bindings_storage(AedxBindings* b, int n) { (void)b; (void)n; return aedx_no(); }
AedxPipeline* aedx_pipeline_create(AedxDevice* d, AedxTarget* t, const void* v, size_t vl, const void* p, size_t pl) {
    (void)d; (void)t; (void)v; (void)vl; (void)p; (void)pl; aedx_no(); return NULL;
}
AedxPipeline* aedx_pipeline_create_ex(AedxDevice* d, AedxTarget* t, const void* v, size_t vl, const void* p,
                                      size_t pl, const AedxLayout* l, int pb, const AedxBindings* b) {
    (void)d; (void)t; (void)v; (void)vl; (void)p; (void)pl; (void)l; (void)pb; (void)b; aedx_no(); return NULL;
}
AedxPipeline* aedx_pipeline_create_state(AedxDevice* d, AedxTarget* t, const void* v, size_t vl, const void* p,
                                         size_t pl, const AedxLayout* l, int pb, const AedxBindings* b,
                                         const AedxState* s) {
    (void)d; (void)t; (void)v; (void)vl; (void)p; (void)pl; (void)l; (void)pb; (void)b; (void)s;
    aedx_no();
    return NULL;
}
void   aedx_pipeline_destroy(AedxPipeline* p) { (void)p; }
AedxTexture* aedx_texture_create(AedxDevice* d, int w, int h) { (void)d; (void)w; (void)h; aedx_no(); return NULL; }
AedxTexture* aedx_texture_create_ex(AedxDevice* d, int w, int h, int m, int l, int r) {
    (void)d; (void)w; (void)h; (void)m; (void)l; (void)r; aedx_no(); return NULL;
}
void   aedx_texture_destroy(AedxTexture* t) { (void)t; }
int    aedx_texture_mip_levels(const AedxTexture* t) { (void)t; return 0; }
AedxTexture* aedx_texture_create_3d(AedxDevice* d, int w, int h, int z, int l, int r) {
    (void)d; (void)w; (void)h; (void)z; (void)l; (void)r; aedx_no(); return NULL;
}
int    aedx_texture_depth(const AedxTexture* t) { (void)t; return 0; }
int    aedx_texture_upload(AedxTexture* t, const void* p, size_t n) { (void)t; (void)p; (void)n; return aedx_no(); }
AedxMaterial* aedx_material_create(AedxPipeline* p) { (void)p; aedx_no(); return NULL; }
void   aedx_material_destroy(AedxMaterial* m) { (void)m; }
int    aedx_material_set_uniform(AedxMaterial* m, int b, const void* d, size_t n) { (void)m; (void)b; (void)d; (void)n; return aedx_no(); }
int    aedx_material_set_texture(AedxMaterial* m, int b, AedxTexture* t) { (void)m; (void)b; (void)t; return aedx_no(); }
int    aedx_material_set_target(AedxMaterial* m, int b, AedxTarget* t, int z) { (void)m; (void)b; (void)t; (void)z; return aedx_no(); }
int    aedx_material_set_buffer(AedxMaterial* m, int b, AedxBuffer* f) { (void)m; (void)b; (void)f; return aedx_no(); }
int    aedx_target_set_push(AedxTarget* t, const void* d, size_t n) { (void)t; (void)d; (void)n; return aedx_no(); }
int    aedx_batch_reset(AedxTarget* t) { (void)t; return aedx_no(); }
int    aedx_batch_add(AedxTarget* t, AedxMaterial* m, int f, int c) { (void)t; (void)m; (void)f; (void)c; return aedx_no(); }
int    aedx_batch_count(const AedxTarget* t) { (void)t; return 0; }
int    aedx_draw(AedxTarget* t, AedxPipeline* p, AedxMaterial* m, float r, float g, float b, float a) {
    (void)t; (void)p; (void)m; (void)r; (void)g; (void)b; (void)a; return aedx_no();
}
int    aedx_submit(AedxTarget* t, AedxPipeline* p, AedxMaterial* m, float r, float g, float b, float a) {
    (void)t; (void)p; (void)m; (void)r; (void)g; (void)b; (void)a; return aedx_no();
}
int    aedx_wait_all(AedxTarget* t) { (void)t; return aedx_no(); }
int    aedx_read_rgba(AedxTarget* t, void* o, size_t n) { (void)t; (void)o; (void)n; return aedx_no(); }
int    aedx_read_rgba8(AedxTarget* t, void* o, size_t n) { (void)t; (void)o; (void)n; return aedx_no(); }
AedxBuffer* aedx_buffer_create(AedxDevice* d, size_t n) { (void)d; (void)n; aedx_no(); return NULL; }
void   aedx_buffer_destroy(AedxBuffer* b) { (void)b; }
size_t aedx_buffer_size(const AedxBuffer* b) { (void)b; return 0; }
int    aedx_buffer_write(AedxBuffer* b, size_t o, const void* d, size_t n) { (void)b; (void)o; (void)d; (void)n; return aedx_no(); }
int    aedx_buffer_read(AedxBuffer* b, size_t o, void* d, size_t n) { (void)b; (void)o; (void)d; (void)n; return aedx_no(); }
AedxCompute* aedx_compute_create(AedxDevice* d, const void* c, size_t n, const AedxBindings* b, int p) {
    (void)d; (void)c; (void)n; (void)b; (void)p; aedx_no(); return NULL;
}
void   aedx_compute_destroy(AedxCompute* c) { (void)c; }
int    aedx_compute_set_buffer(AedxCompute* c, int b, AedxBuffer* f) { (void)c; (void)b; (void)f; return aedx_no(); }
int    aedx_compute_set_texture(AedxCompute* c, int b, AedxTexture* t) { (void)c; (void)b; (void)t; return aedx_no(); }
int    aedx_compute_set_push(AedxCompute* c, const void* d, size_t n) { (void)c; (void)d; (void)n; return aedx_no(); }
int    aedx_compute_set_timeout_ms(AedxCompute* c, int ms) { (void)c; (void)ms; return aedx_no(); }
int    aedx_dispatch(AedxCompute* c, int x, int y, int z) { (void)c; (void)x; (void)y; (void)z; return aedx_no(); }
int    aedx_dispatch_async(AedxCompute* c, int x, int y, int z) { (void)c; (void)x; (void)y; (void)z; return aedx_no(); }
int    aedx_compute_wait(AedxCompute* c) { (void)c; return aedx_no(); }
int    aedx_batch_add_instanced(AedxTarget* t, AedxMaterial* m, int f, int c, int fi, int n) {
    (void)t; (void)m; (void)f; (void)c; (void)fi; (void)n; return aedx_no();
}
int    aedx_batch_add_indirect(AedxTarget* t, AedxMaterial* m, AedxBuffer* b, int o, int n) {
    (void)t; (void)m; (void)b; (void)o; (void)n; return aedx_no();
}
int    aedx_batch_set_offset(AedxTarget* t, int i, int b, int o) { (void)t; (void)i; (void)b; (void)o; return aedx_no(); }
int    aedx_uniform_offset_alignment(const AedxDevice* d) { (void)d; return 0; }
int    aedx_bindings_uniform_dynamic(AedxBindings* b, int n, int s) { (void)b; (void)n; (void)s; return aedx_no(); }
int    aedx_target_set_stream(AedxTarget* t, int b, AedxBuffer* buf) { (void)t; (void)b; (void)buf; return aedx_no(); }
int    aedx_target_set_instances(AedxTarget* t, int n) { (void)t; (void)n; return aedx_no(); }
int    aedx_target_instances(const AedxTarget* t) { (void)t; return 0; }
int    aedx_target_set_timing(AedxTarget* t, int on) { (void)t; (void)on; return aedx_no(); }
double aedx_target_gpu_ms(const AedxTarget* t) { (void)t; return -1.0; }
int    aedx_compute_set_timing(AedxCompute* c, int on) { (void)c; (void)on; return aedx_no(); }
int    aedx_material_set_target_depth_compare(AedxMaterial* m, int b, AedxTarget* t, int op) {
    (void)m; (void)b; (void)t; (void)op; return aedx_no();
}
int    aedx_pipeline_set_target_depth_compare(AedxPipeline* p, int b, AedxTarget* t, int op) {
    (void)p; (void)b; (void)t; (void)op; return aedx_no();
}
int    aedx_target_depth_linear(const AedxTarget* t) { (void)t; return 0; }
AedxTexture* aedx_texture_create_cube(AedxDevice* d, int s, int m, int l) {
    (void)d; (void)s; (void)m; (void)l; aedx_no(); return NULL;
}
AedxTexture* aedx_texture_create_array(AedxDevice* d, int w, int h, int n, int m, int l, int r) {
    (void)d; (void)w; (void)h; (void)n; (void)m; (void)l; (void)r; aedx_no(); return NULL;
}
AedxTexture* aedx_texture_create_storage(AedxDevice* d, int w, int h, int z, int f) {
    (void)d; (void)w; (void)h; (void)z; (void)f; aedx_no(); return NULL;
}
int    aedx_texture_layers(const AedxTexture* t) { (void)t; return 0; }
AedxTexture* aedx_texture_create_cube_format(AedxDevice* d, int s, int f, int m, int l) {
    (void)d; (void)s; (void)f; (void)m; (void)l; aedx_no(); return NULL;
}
AedxTexture* aedx_texture_create_array_format(AedxDevice* d, int w, int h, int n, int f, int m, int l, int r) {
    (void)d; (void)w; (void)h; (void)n; (void)f; (void)m; (void)l; (void)r; aedx_no(); return NULL;
}
int    aedx_batch_set_stencil_ref(AedxTarget* t, int i, int r) { (void)t; (void)i; (void)r; return aedx_no(); }
int    aedx_batch_set_pipeline(AedxTarget* t, int i, AedxPipeline* p) { (void)t; (void)i; (void)p; return aedx_no(); }
AedxTarget* aedx_target_create_layered(AedxDevice* d, int w, int h, int n, int f, int z, int c) {
    (void)d; (void)w; (void)h; (void)n; (void)f; (void)z; (void)c; aedx_no(); return NULL;
}
int    aedx_target_set_layer(AedxTarget* t, int l) { (void)t; (void)l; return aedx_no(); }
int    aedx_target_layers(const AedxTarget* t) { (void)t; return 0; }
AedxTarget* aedx_target_create_layered_ex(AedxDevice* d, int w, int h, int n, int f, int z, int c, int s) {
    (void)d; (void)w; (void)h; (void)n; (void)f; (void)z; (void)c; (void)s; aedx_no(); return NULL;
}
int    aedx_pipeline_set_target_depth_array(AedxPipeline* p, int b, AedxTarget* t, int o) {
    (void)p; (void)b; (void)t; (void)o; return aedx_no();
}
int    aedx_pipeline_set_target_depth_cube(AedxPipeline* p, int b, AedxTarget* t, int o) {
    (void)p; (void)b; (void)t; (void)o; return aedx_no();
}
int    aedx_material_set_target_depth_array(AedxMaterial* m, int b, AedxTarget* t, int o) {
    (void)m; (void)b; (void)t; (void)o; return aedx_no();
}
int    aedx_material_set_target_depth_cube(AedxMaterial* m, int b, AedxTarget* t, int o) {
    (void)m; (void)b; (void)t; (void)o; return aedx_no();
}
int    aedx_pipeline_set_target_array(AedxPipeline* p, int b, AedxTarget* t) { (void)p; (void)b; (void)t; return aedx_no(); }
int    aedx_pipeline_set_target_cube(AedxPipeline* p, int b, AedxTarget* t) { (void)p; (void)b; (void)t; return aedx_no(); }
int    aedx_material_set_target_array(AedxMaterial* m, int b, AedxTarget* t) { (void)m; (void)b; (void)t; return aedx_no(); }
int    aedx_material_set_target_cube(AedxMaterial* m, int b, AedxTarget* t) { (void)m; (void)b; (void)t; return aedx_no(); }
AedxTexture* aedx_texture_create_format(AedxDevice* d, int w, int h, int f, int m, int l, int r, int a) {
    (void)d; (void)w; (void)h; (void)f; (void)m; (void)l; (void)r; (void)a; aedx_no(); return NULL;
}
int    aedx_texture_format_supported(AedxDevice* d, int f) { (void)d; (void)f; return 0; }
int    aedx_texture_anisotropy(const AedxTexture* t) { (void)t; return 0; }
int    aedx_texture_upload_level(AedxTexture* t, int l, const void* p, size_t n) {
    (void)t; (void)l; (void)p; (void)n; return aedx_no();
}
AedxTarget* aedx_target_create_mrt(AedxDevice* d, int w, int h, int n, int f0, int f1, int f2, int f3, int z, int s) {
    (void)d; (void)w; (void)h; (void)n; (void)f0; (void)f1; (void)f2; (void)f3; (void)z; (void)s;
    aedx_no();
    return NULL;
}
int    aedx_target_attachments(const AedxTarget* t) { (void)t; return 0; }
int    aedx_pipeline_set_target_attachment(AedxPipeline* p, int b, AedxTarget* t, int n) {
    (void)p; (void)b; (void)t; (void)n; return aedx_no();
}
int    aedx_material_set_target_attachment(AedxMaterial* m, int b, AedxTarget* t, int n) {
    (void)m; (void)b; (void)t; (void)n; return aedx_no();
}
double aedx_ae_pixel_value_at(void* t, int n, int x, int y, int c) {
    (void)t; (void)n; (void)x; (void)y; (void)c; aedx_no(); return (double)NAN;
}
int    aedx_batch_set_scissor(AedxTarget* t, int i, int x, int y, int w, int h) {
    (void)t; (void)i; (void)x; (void)y; (void)w; (void)h; return aedx_no();
}
int    aedx_batch_set_viewport(AedxTarget* t, int i, float x, float y, float w, float h, float a, float b) {
    (void)t; (void)i; (void)x; (void)y; (void)w; (void)h; (void)a; (void)b; return aedx_no();
}
int    aedx_bindings_storage_texture(AedxBindings* b, int n) { (void)b; (void)n; return aedx_no(); }
int    aedx_compute_set_storage_texture(AedxCompute* c, int b, AedxTexture* t) {
    (void)c; (void)b; (void)t; return aedx_no();
}
double aedx_compute_gpu_ms(const AedxCompute* c) { (void)c; return -1.0; }
AedxSwapchain* aedx_swapchain_create(AedxDevice* d, int k, void* dp, void* w, int x, int y) {
    (void)d; (void)k; (void)dp; (void)w; (void)x; (void)y; aedx_no(); return NULL;
}
void   aedx_swapchain_destroy(AedxSwapchain* s) { (void)s; }
int    aedx_swapchain_resize(AedxSwapchain* s, int w, int h) { (void)s; (void)w; (void)h; return aedx_no(); }
int    aedx_swapchain_set_vsync(AedxSwapchain* s, int on) { (void)s; (void)on; return aedx_no(); }
int    aedx_swapchain_width(const AedxSwapchain* s) { (void)s; return 0; }
int    aedx_swapchain_height(const AedxSwapchain* s) { (void)s; return 0; }
int    aedx_swapchain_format(const AedxSwapchain* s) { (void)s; return 0; }
long long aedx_swapchain_presented(const AedxSwapchain* s) { (void)s; return 0; }
int    aedx_present(AedxSwapchain* s, AedxTarget* t) { (void)s; (void)t; return aedx_no(); }

int    aedx_ae_verts_reserve_n(void* t, int c, int f) { (void)t; (void)c; (void)f; return aedx_no(); }
int    aedx_ae_verts_reserve(void* t, int c) { (void)t; (void)c; return aedx_no(); }
int    aedx_ae_verts_set_float(void* t, int i, double v) { (void)t; (void)i; (void)v; return aedx_no(); }
int    aedx_ae_verts_set(void* t, int i, double x, double y, double r, double g, double b) {
    (void)t; (void)i; (void)x; (void)y; (void)r; (void)g; (void)b; return aedx_no();
}
int    aedx_ae_indices_reserve_ex(void* t, int c, int b) { (void)t; (void)c; (void)b; return aedx_no(); }
int    aedx_ae_indices_reserve(void* t, int c) { (void)t; (void)c; return aedx_no(); }
int    aedx_ae_indices_set(void* t, int i, int v) { (void)t; (void)i; (void)v; return aedx_no(); }
int    aedx_ae_push_floats(void* t, int c) { (void)t; (void)c; return aedx_no(); }
int    aedx_ae_push_float(void* t, int i, double v) { (void)t; (void)i; (void)v; return aedx_no(); }
int64_t aedx_ae_pixel(void* t, int x, int y) { (void)t; (void)x; (void)y; aedx_no(); return -1; }
double aedx_ae_pixel_value(void* t, int x, int y, int c) { (void)t; (void)x; (void)y; (void)c; aedx_no(); return (double)NAN; }
int    aedx_ae_save_ppm(void* t, const char* p) { (void)t; (void)p; return aedx_no(); }
int    aedx_ae_uniform_floats(void* p, int b, int c) { (void)p; (void)b; (void)c; return aedx_no(); }
int    aedx_ae_uniform_float(void* p, int b, int i, double v) { (void)p; (void)b; (void)i; (void)v; return aedx_no(); }
int    aedx_ae_material_floats(void* m, int b, int c) { (void)m; (void)b; (void)c; return aedx_no(); }
int    aedx_ae_material_float(void* m, int b, int i, double v) { (void)m; (void)b; (void)i; (void)v; return aedx_no(); }
int    aedx_ae_set_uniform(void* p, int b, const void* d, int n) { (void)p; (void)b; (void)d; (void)n; return aedx_no(); }
int    aedx_ae_set_texture(void* p, int b, void* t) { (void)p; (void)b; (void)t; return aedx_no(); }
int    aedx_ae_set_target(void* p, int b, void* t, int z) { (void)p; (void)b; (void)t; (void)z; return aedx_no(); }
int    aedx_ae_set_buffer(void* p, int b, void* f) { (void)p; (void)b; (void)f; return aedx_no(); }
int    aedx_ae_compute_push_float(void* c, int i, double v) { (void)c; (void)i; (void)v; return aedx_no(); }
int    aedx_ae_compute_push_int(void* c, int i, int v) { (void)c; (void)i; (void)v; return aedx_no(); }

#endif /* _WIN32 */

/* ------------------------------------------------------------------------ */
/* Aether-facing entry points                                                */
/* ------------------------------------------------------------------------ */
/* Signatures match what aetherc emits for the externs in module.ae: ptr is
 * void*, string is const char*, int is int and float is double. */

int         aedx_ae_available(void)          { return aedx_available(); }
const char* aedx_ae_last_error(void)         { return aedx_last_error(); }
const char* aedx_ae_device_name(void)        { return aedx_device_name(); }
void*       aedx_ae_device_create(void)      { return (void*)aedx_device_create(); }
void        aedx_ae_device_destroy(void* d)  { aedx_device_destroy((AedxDevice*)d); }
int         aedx_ae_device_is_warp(void* d)  { return aedx_device_is_warp((const AedxDevice*)d); }
int         aedx_ae_debug_message_count(void* d) { return aedx_debug_message_count((const AedxDevice*)d); }

void* aedx_ae_target_create(void* d, int w, int h) { return (void*)aedx_target_create((AedxDevice*)d, w, h); }
void* aedx_ae_target_create_ex(void* d, int w, int h, int depth, int samples) {
    return (void*)aedx_target_create_ex((AedxDevice*)d, w, h, depth, samples);
}
void* aedx_ae_target_create_format(void* d, int w, int h, int format, int depth, int samples) {
    return (void*)aedx_target_create_format((AedxDevice*)d, w, h, format, depth, samples);
}
void  aedx_ae_target_destroy(void* t)        { aedx_target_destroy((AedxTarget*)t); }
int   aedx_ae_target_width(void* t)          { return aedx_target_width((const AedxTarget*)t); }
int   aedx_ae_target_height(void* t)         { return aedx_target_height((const AedxTarget*)t); }
int   aedx_ae_target_format(void* t)         { return aedx_target_format((const AedxTarget*)t); }
int   aedx_ae_target_bytes_per_pixel(void* t) { return aedx_target_bytes_per_pixel((const AedxTarget*)t); }
int   aedx_ae_target_has_depth(void* t)      { return aedx_target_has_depth((const AedxTarget*)t); }
int   aedx_ae_target_samples(void* t)        { return aedx_target_samples((const AedxTarget*)t); }
int   aedx_ae_target_resize(void* t, int w, int h) { return aedx_target_resize((AedxTarget*)t, w, h); }
int   aedx_ae_target_set_readback(void* t, int on) { return aedx_target_set_readback((AedxTarget*)t, on); }
int   aedx_ae_target_readback(void* t)       { return aedx_target_readback((const AedxTarget*)t); }
int   aedx_ae_target_set_frames(void* t, int n) { return aedx_target_set_frames((AedxTarget*)t, n); }
int   aedx_ae_target_frames(void* t)         { return aedx_target_frames((const AedxTarget*)t); }
int   aedx_ae_target_set_timeout_ms(void* t, int ms) { return aedx_target_set_timeout_ms((AedxTarget*)t, ms); }
int   aedx_ae_rgba_size(void* t) {
    size_t n = aedx_rgba_size((const AedxTarget*)t);
    return n > 0x7fffffffu ? 0x7fffffff : (int)n;
}

void* aedx_ae_pipeline_create(void* d, void* t, const char* vs, int vl, const char* ps, int pl) {
    if (vl < 0 || pl < 0) { aedx_fail(AEDX_ERR_SHADER, "negative shader length"); return NULL; }
    return (void*)aedx_pipeline_create((AedxDevice*)d, (AedxTarget*)t, vs, (size_t)vl, ps, (size_t)pl);
}
void* aedx_ae_pipeline_create_ex(void* d, void* t, const char* vs, int vl, const char* ps, int pl,
                                 void* layout, int push_bytes, void* bindings) {
    if (vl < 0 || pl < 0) { aedx_fail(AEDX_ERR_SHADER, "negative shader length"); return NULL; }
    return (void*)aedx_pipeline_create_ex((AedxDevice*)d, (AedxTarget*)t, vs, (size_t)vl, ps, (size_t)pl,
                                          (const AedxLayout*)layout, push_bytes, (const AedxBindings*)bindings);
}
void  aedx_ae_pipeline_destroy(void* p)      { aedx_pipeline_destroy((AedxPipeline*)p); }

int   aedx_ae_state_topology(void* s, int topology) { return aedx_state_topology((AedxState*)s, topology); }
int   aedx_ae_batch_set_scissor(void* t, int item, int x, int y, int w, int h) {
    return aedx_batch_set_scissor((AedxTarget*)t, item, x, y, w, h);
}
int   aedx_ae_batch_set_viewport(void* t, int item, double x, double y, double w, double h,
                                double min_depth, double max_depth) {
    return aedx_batch_set_viewport((AedxTarget*)t, item, (float)x, (float)y, (float)w, (float)h,
                                  (float)min_depth, (float)max_depth);
}
void* aedx_ae_state_create(void) { return (void*)aedx_state_create(); }
void  aedx_ae_state_destroy(void* s) { aedx_state_destroy((AedxState*)s); }
int   aedx_ae_state_blend(void* s, int mode) { return aedx_state_blend((AedxState*)s, mode); }
int   aedx_ae_state_cull(void* s, int mode) { return aedx_state_cull((AedxState*)s, mode); }
int   aedx_ae_state_depth(void* s, int op, int write) { return aedx_state_depth((AedxState*)s, op, write); }
void* aedx_ae_pipeline_create_state(void* d, void* t, const char* vs, int vl, const char* fs, int fl,
                                   void* layout, int push_bytes, void* bindings, void* state) {
    if (vl < 0 || fl < 0) { aedx_fail(AEDX_ERR_SHADER, "negative shader length"); return NULL; }
    return (void*)aedx_pipeline_create_state((AedxDevice*)d, (AedxTarget*)t, vs, (size_t)vl, fs, (size_t)fl,
                                            (const AedxLayout*)layout, push_bytes,
                                            (const AedxBindings*)bindings, (const AedxState*)state);
}

void* aedx_ae_layout_create(void)            { return (void*)aedx_layout_create(); }
void  aedx_ae_layout_destroy(void* l)        { aedx_layout_destroy((AedxLayout*)l); }
int   aedx_ae_layout_binding(void* l, int b, int stride, int per_instance) {
    return aedx_layout_binding((AedxLayout*)l, b, stride, per_instance);
}
int   aedx_ae_layout_attr(void* l, int location, int b, int format, int offset) {
    return aedx_layout_attr((AedxLayout*)l, location, b, format, offset);
}
void* aedx_ae_bindings_create(void)          { return (void*)aedx_bindings_create(); }
void  aedx_ae_bindings_destroy(void* b)      { aedx_bindings_destroy((AedxBindings*)b); }
int   aedx_ae_bindings_uniform(void* b, int n) { return aedx_bindings_uniform((AedxBindings*)b, n); }
int   aedx_ae_bindings_texture(void* b, int n) { return aedx_bindings_texture((AedxBindings*)b, n); }
int   aedx_ae_bindings_storage(void* b, int n) { return aedx_bindings_storage((AedxBindings*)b, n); }

void* aedx_ae_texture_create(void* d, int w, int h) { return (void*)aedx_texture_create((AedxDevice*)d, w, h); }
void* aedx_ae_texture_create_ex(void* d, int w, int h, int mipmapped, int linear, int repeat) {
    return (void*)aedx_texture_create_ex((AedxDevice*)d, w, h, mipmapped, linear, repeat);
}
void  aedx_ae_texture_destroy(void* t)       { aedx_texture_destroy((AedxTexture*)t); }
int   aedx_ae_texture_mip_levels(void* t)    { return aedx_texture_mip_levels((const AedxTexture*)t); }
void* aedx_ae_texture_create_3d(void* d, int w, int h, int depth, int linear, int repeat) {
    return (void*)aedx_texture_create_3d((AedxDevice*)d, w, h, depth, linear, repeat);
}
int   aedx_ae_texture_depth(void* t)         { return aedx_texture_depth((const AedxTexture*)t); }
int   aedx_ae_texture_upload(void* t, const void* rgba, int len) {
    if (len < 0) return aedx_fail(AEDX_ERR_ARG, "negative pixel length");
    return aedx_texture_upload((AedxTexture*)t, rgba, (size_t)len);
}

void* aedx_ae_material_create(void* p)       { return (void*)aedx_material_create((AedxPipeline*)p); }
void  aedx_ae_material_destroy(void* m)      { aedx_material_destroy((AedxMaterial*)m); }
int   aedx_ae_material_set_texture(void* m, int b, void* t) {
    return aedx_material_set_texture((AedxMaterial*)m, b, (AedxTexture*)t);
}
int   aedx_ae_material_set_target(void* m, int b, void* t, int depth) {
    return aedx_material_set_target((AedxMaterial*)m, b, (AedxTarget*)t, depth);
}
int   aedx_ae_material_set_uniform(void* m, int b, const void* data, int len) {
    if (len < 0) return aedx_fail(AEDX_ERR_ARG, "negative uniform length");
    return aedx_material_set_uniform((AedxMaterial*)m, b, data, (size_t)len);
}
int   aedx_ae_material_set_buffer(void* m, int b, void* buf) {
    return aedx_material_set_buffer((AedxMaterial*)m, b, (AedxBuffer*)buf);
}

int   aedx_ae_set_push(void* t, const void* data, int len) {
    if (len < 0) return aedx_fail(AEDX_ERR_ARG, "negative push length");
    return aedx_target_set_push((AedxTarget*)t, data, (size_t)len);
}
int   aedx_ae_batch_reset(void* t)           { return aedx_batch_reset((AedxTarget*)t); }
int   aedx_ae_batch_add(void* t, void* m, int first, int count) {
    return aedx_batch_add((AedxTarget*)t, (AedxMaterial*)m, first, count);
}
int   aedx_ae_batch_count(void* t)           { return aedx_batch_count((const AedxTarget*)t); }
int   aedx_ae_draw(void* t, void* p, double r, double g, double b, double a) {
    return aedx_draw((AedxTarget*)t, (AedxPipeline*)p, NULL, (float)r, (float)g, (float)b, (float)a);
}
int   aedx_ae_draw_material(void* t, void* p, void* m, double r, double g, double b, double a) {
    return aedx_draw((AedxTarget*)t, (AedxPipeline*)p, (AedxMaterial*)m, (float)r, (float)g, (float)b, (float)a);
}
int   aedx_ae_submit(void* t, void* p, double r, double g, double b, double a) {
    return aedx_submit((AedxTarget*)t, (AedxPipeline*)p, NULL, (float)r, (float)g, (float)b, (float)a);
}
int   aedx_ae_submit_material(void* t, void* p, void* m, double r, double g, double b, double a) {
    return aedx_submit((AedxTarget*)t, (AedxPipeline*)p, (AedxMaterial*)m, (float)r, (float)g, (float)b, (float)a);
}
int   aedx_ae_wait_all(void* t)              { return aedx_wait_all((AedxTarget*)t); }
int   aedx_ae_copy_rgba(void* t, void* dest, int len) {
    if (len < 0) return aedx_fail(AEDX_ERR_ARG, "negative destination length");
    return aedx_read_rgba((AedxTarget*)t, dest, (size_t)len);
}
int   aedx_ae_copy_rgba8(void* t, void* dest, int len) {
    if (len < 0) return aedx_fail(AEDX_ERR_ARG, "negative destination length");
    return aedx_read_rgba8((AedxTarget*)t, dest, (size_t)len);
}

void* aedx_ae_buffer_create(void* d, int bytes) {
    if (bytes < 0) { aedx_fail(AEDX_ERR_ARG, "negative buffer size"); return NULL; }
    return (void*)aedx_buffer_create((AedxDevice*)d, (size_t)bytes);
}
void  aedx_ae_buffer_destroy(void* b)        { aedx_buffer_destroy((AedxBuffer*)b); }
int   aedx_ae_buffer_size(void* b) {
    size_t n = aedx_buffer_size((const AedxBuffer*)b);
    return n > 0x7fffffffu ? 0x7fffffff : (int)n;
}
int   aedx_ae_buffer_write(void* b, int offset, const void* data, int len) {
    if (offset < 0 || len < 0) return aedx_fail(AEDX_ERR_ARG, "negative offset or length");
    return aedx_buffer_write((AedxBuffer*)b, (size_t)offset, data, (size_t)len);
}
int   aedx_ae_buffer_read(void* b, int offset, void* dest, int len) {
    if (offset < 0 || len < 0) return aedx_fail(AEDX_ERR_ARG, "negative offset or length");
    return aedx_buffer_read((AedxBuffer*)b, (size_t)offset, dest, (size_t)len);
}
int   aedx_ae_buffer_set_float(void* b, int index, double value) {
    float f = (float)value;
    if (index < 0) return aedx_fail(AEDX_ERR_ARG, "negative index");
    return aedx_buffer_write((AedxBuffer*)b, (size_t)index * 4u, &f, sizeof(f));
}
double aedx_ae_buffer_float(void* b, int index) {
    float f = 0.0f;
    if (index < 0) { aedx_fail(AEDX_ERR_ARG, "negative index"); return (double)NAN; }
    if (aedx_buffer_read((AedxBuffer*)b, (size_t)index * 4u, &f, sizeof(f)) != AEDX_OK) return (double)NAN;
    return (double)f;
}
int   aedx_ae_buffer_set_int(void* b, int index, int value) {
    int32_t v = (int32_t)value;
    if (index < 0) return aedx_fail(AEDX_ERR_ARG, "negative index");
    return aedx_buffer_write((AedxBuffer*)b, (size_t)index * 4u, &v, sizeof(v));
}
int   aedx_ae_buffer_int(void* b, int index) {
    int32_t v = 0;
    if (index < 0) { aedx_fail(AEDX_ERR_ARG, "negative index"); return 0; }
    if (aedx_buffer_read((AedxBuffer*)b, (size_t)index * 4u, &v, sizeof(v)) != AEDX_OK) return 0;
    return (int)v;
}

void* aedx_ae_compute_create(void* d, const char* cs, int len, void* bindings, int push_bytes) {
    if (len < 0) { aedx_fail(AEDX_ERR_SHADER, "negative shader length"); return NULL; }
    return (void*)aedx_compute_create((AedxDevice*)d, cs, (size_t)len, (const AedxBindings*)bindings, push_bytes);
}
void  aedx_ae_compute_destroy(void* c)       { aedx_compute_destroy((AedxCompute*)c); }
int   aedx_ae_compute_set_buffer(void* c, int b, void* buf) {
    return aedx_compute_set_buffer((AedxCompute*)c, b, (AedxBuffer*)buf);
}
int   aedx_ae_compute_set_texture(void* c, int b, void* t) {
    return aedx_compute_set_texture((AedxCompute*)c, b, (AedxTexture*)t);
}
int   aedx_ae_compute_set_push(void* c, const void* data, int len) {
    if (len < 0) return aedx_fail(AEDX_ERR_ARG, "negative push length");
    return aedx_compute_set_push((AedxCompute*)c, data, (size_t)len);
}
int   aedx_ae_compute_set_timeout_ms(void* c, int ms) { return aedx_compute_set_timeout_ms((AedxCompute*)c, ms); }
int   aedx_ae_dispatch(void* c, int x, int y, int z) { return aedx_dispatch((AedxCompute*)c, x, y, z); }
int   aedx_ae_dispatch_async(void* c, int x, int y, int z) { return aedx_dispatch_async((AedxCompute*)c, x, y, z); }
int   aedx_ae_compute_wait(void* c)          { return aedx_compute_wait((AedxCompute*)c); }
int   aedx_ae_batch_add_instanced(void* t, void* m, int first, int count, int first_instance, int instances) {
    return aedx_batch_add_instanced((AedxTarget*)t, (AedxMaterial*)m, first, count, first_instance, instances);
}
int   aedx_ae_batch_add_indirect(void* t, void* m, void* buf, int offset, int draws) {
    return aedx_batch_add_indirect((AedxTarget*)t, (AedxMaterial*)m, (AedxBuffer*)buf, offset, draws);
}
int   aedx_ae_batch_set_offset(void* t, int item, int binding, int offset) {
    return aedx_batch_set_offset((AedxTarget*)t, item, binding, offset);
}
int   aedx_ae_uniform_offset_alignment(void* d) { return aedx_uniform_offset_alignment((const AedxDevice*)d); }
int   aedx_ae_bindings_uniform_dynamic(void* b, int binding, int bytes) {
    return aedx_bindings_uniform_dynamic((AedxBindings*)b, binding, bytes);
}
int   aedx_ae_vertex_stream(void* t, int binding, void* buf) {
    return aedx_target_set_stream((AedxTarget*)t, binding, (AedxBuffer*)buf);
}
int   aedx_ae_target_set_instances(void* t, int count) { return aedx_target_set_instances((AedxTarget*)t, count); }
int   aedx_ae_target_instances(void* t) { return aedx_target_instances((const AedxTarget*)t); }
int   aedx_ae_target_set_timing(void* t, int on) { return aedx_target_set_timing((AedxTarget*)t, on); }
double aedx_ae_target_gpu_ms(void* t) { return aedx_target_gpu_ms((const AedxTarget*)t); }
int   aedx_ae_compute_set_timing(void* c, int on) { return aedx_compute_set_timing((AedxCompute*)c, on); }
int   aedx_ae_set_target_depth_compare(void* p, int binding, void* t, int op) {
    return aedx_pipeline_set_target_depth_compare((AedxPipeline*)p, binding, (AedxTarget*)t, op);
}
int   aedx_ae_material_set_target_depth_compare(void* m, int binding, void* t, int op) {
    return aedx_material_set_target_depth_compare((AedxMaterial*)m, binding, (AedxTarget*)t, op);
}
int   aedx_ae_target_depth_linear(void* t) { return aedx_target_depth_linear((const AedxTarget*)t); }
void* aedx_ae_texture_create_cube(void* d, int size, int mipmapped, int linear) {
    return (void*)aedx_texture_create_cube((AedxDevice*)d, size, mipmapped, linear);
}
void* aedx_ae_texture_create_array(void* d, int w, int h, int layers, int mipmapped, int linear, int repeat) {
    return (void*)aedx_texture_create_array((AedxDevice*)d, w, h, layers, mipmapped, linear, repeat);
}
void* aedx_ae_texture_create_storage(void* d, int w, int h, int depth, int format) {
    return (void*)aedx_texture_create_storage((AedxDevice*)d, w, h, depth, format);
}
int   aedx_ae_texture_layers(void* tex) { return aedx_texture_layers((const AedxTexture*)tex); }
void* aedx_ae_texture_create_cube_format(void* d, int size, int format, int mipmapped, int linear) {
    return (void*)aedx_texture_create_cube_format((AedxDevice*)d, size, format, mipmapped, linear);
}
void* aedx_ae_texture_create_array_format(void* d, int w, int h, int layers, int format, int mipmapped,
                                          int linear, int repeat) {
    return (void*)aedx_texture_create_array_format((AedxDevice*)d, w, h, layers, format, mipmapped, linear,
                                                   repeat);
}
int   aedx_ae_state_stencil(void* st, int compare, int ref, int pass_op, int fail_op, int depth_fail_op,
                            int read_mask, int write_mask) {
    return aedx_state_stencil((AedxState*)st, compare, ref, pass_op, fail_op, depth_fail_op, read_mask,
                              write_mask);
}
int   aedx_ae_state_color_mask(void* st, int mask) { return aedx_state_color_mask((AedxState*)st, mask); }
int   aedx_ae_batch_set_pipeline(void* t, int item, void* p) {
    return aedx_batch_set_pipeline((AedxTarget*)t, item, (AedxPipeline*)p);
}
int   aedx_ae_batch_set_stencil_ref(void* t, int item, int ref) {
    return aedx_batch_set_stencil_ref((AedxTarget*)t, item, ref);
}
void* aedx_ae_target_create_layered(void* d, int w, int h, int layers, int format, int depth, int cube) {
    return (void*)aedx_target_create_layered((AedxDevice*)d, w, h, layers, format, depth, cube);
}
int   aedx_ae_target_set_layer(void* t, int layer) { return aedx_target_set_layer((AedxTarget*)t, layer); }
int   aedx_ae_target_layers(void* t) { return aedx_target_layers((const AedxTarget*)t); }
void* aedx_ae_target_create_layered_ex(void* d, int w, int h, int layers, int format, int depth, int cube,
                                       int samples) {
    return (void*)aedx_target_create_layered_ex((AedxDevice*)d, w, h, layers, format, depth, cube, samples);
}
int   aedx_ae_set_target_depth_array(void* p, int binding, void* t, int op) {
    return aedx_pipeline_set_target_depth_array((AedxPipeline*)p, binding, (AedxTarget*)t, op);
}
int   aedx_ae_set_target_depth_cube(void* p, int binding, void* t, int op) {
    return aedx_pipeline_set_target_depth_cube((AedxPipeline*)p, binding, (AedxTarget*)t, op);
}
int   aedx_ae_material_set_target_depth_array(void* m, int binding, void* t, int op) {
    return aedx_material_set_target_depth_array((AedxMaterial*)m, binding, (AedxTarget*)t, op);
}
int   aedx_ae_material_set_target_depth_cube(void* m, int binding, void* t, int op) {
    return aedx_material_set_target_depth_cube((AedxMaterial*)m, binding, (AedxTarget*)t, op);
}
int   aedx_ae_set_target_array(void* p, int binding, void* t) {
    return aedx_pipeline_set_target_array((AedxPipeline*)p, binding, (AedxTarget*)t);
}
int   aedx_ae_set_target_cube(void* p, int binding, void* t) {
    return aedx_pipeline_set_target_cube((AedxPipeline*)p, binding, (AedxTarget*)t);
}
int   aedx_ae_material_set_target_array(void* m, int binding, void* t) {
    return aedx_material_set_target_array((AedxMaterial*)m, binding, (AedxTarget*)t);
}
int   aedx_ae_material_set_target_cube(void* m, int binding, void* t) {
    return aedx_material_set_target_cube((AedxMaterial*)m, binding, (AedxTarget*)t);
}
void* aedx_ae_texture_create_format(void* d, int w, int h, int format, int mipmapped, int linear, int repeat,
                                    int anisotropy) {
    return (void*)aedx_texture_create_format((AedxDevice*)d, w, h, format, mipmapped, linear, repeat, anisotropy);
}
int   aedx_ae_texture_format_supported(void* d, int format) {
    return aedx_texture_format_supported((AedxDevice*)d, format);
}
int   aedx_ae_texture_anisotropy(void* tex) { return aedx_texture_anisotropy((const AedxTexture*)tex); }
int   aedx_ae_texture_upload_level(void* tex, int level, const void* data, int len) {
    if (len < 0) return aedx_fail(AEDX_ERR_ARG, "negative length");
    return aedx_texture_upload_level((AedxTexture*)tex, level, data, (size_t)len);
}
void* aedx_ae_target_create_mrt(void* d, int w, int h, int count, int f0, int f1, int f2, int f3,
                                int depth, int samples) {
    return (void*)aedx_target_create_mrt((AedxDevice*)d, w, h, count, f0, f1, f2, f3, depth, samples);
}
int   aedx_ae_target_attachments(void* t) { return aedx_target_attachments((const AedxTarget*)t); }
int   aedx_ae_set_target_attachment(void* p, int binding, void* t, int n) {
    return aedx_pipeline_set_target_attachment((AedxPipeline*)p, binding, (AedxTarget*)t, n);
}
int   aedx_ae_material_set_target_attachment(void* m, int binding, void* t, int n) {
    return aedx_material_set_target_attachment((AedxMaterial*)m, binding, (AedxTarget*)t, n);
}
int   aedx_ae_bindings_storage_texture(void* b, int binding) {
    return aedx_bindings_storage_texture((AedxBindings*)b, binding);
}
int   aedx_ae_compute_set_storage_texture(void* c, int binding, void* tex) {
    return aedx_compute_set_storage_texture((AedxCompute*)c, binding, (AedxTexture*)tex);
}
double aedx_ae_compute_gpu_ms(void* c) { return aedx_compute_gpu_ms((const AedxCompute*)c); }

void* aedx_ae_swapchain_create(void* d, int kind, void* display, void* window, int w, int h) {
    return (void*)aedx_swapchain_create((AedxDevice*)d, kind, display, window, w, h);
}
void  aedx_ae_swapchain_destroy(void* s)     { aedx_swapchain_destroy((AedxSwapchain*)s); }
int   aedx_ae_swapchain_resize(void* s, int w, int h) { return aedx_swapchain_resize((AedxSwapchain*)s, w, h); }
int   aedx_ae_swapchain_set_vsync(void* s, int on) { return aedx_swapchain_set_vsync((AedxSwapchain*)s, on); }
int   aedx_ae_swapchain_width(void* s)       { return aedx_swapchain_width((const AedxSwapchain*)s); }
int   aedx_ae_swapchain_height(void* s)      { return aedx_swapchain_height((const AedxSwapchain*)s); }
int   aedx_ae_swapchain_format(void* s)      { return aedx_swapchain_format((const AedxSwapchain*)s); }
int   aedx_ae_swapchain_presented(void* s) {
    long long n = aedx_swapchain_presented((const AedxSwapchain*)s);
    return n > 0x7fffffffLL ? 0x7fffffff : (int)n;
}
int   aedx_ae_present(void* s, void* t)      { return aedx_present((AedxSwapchain*)s, (AedxTarget*)t); }
