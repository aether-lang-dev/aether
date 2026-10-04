/* contrib/d3d12: GPU rendering, compute and presentation with Direct3D 12.
 *
 * The same shape as contrib/vulkan: a device, offscreen targets, pipelines
 * with vertex layouts, bindings, textures and materials, batches, frames in
 * flight, colour formats, readback, compute, and swapchains over a window
 * someone else owns. A program that knows one knows the other; what differs
 * is the shading language and the coordinate conventions (below).
 *
 * Direct3D 12, DXGI and the HLSL compiler are opened at RUNTIME (d3d12.dll,
 * dxgi.dll, d3dcompiler_47.dll), so nothing links against them: a program
 * using this module builds on every platform, and aedx_available() reports 0
 * where there is no Direct3D 12 (any system other than Windows 10 or later).
 * The device is the first hardware adapter that supports feature level 11_0,
 * or WARP, the software rasteriser Windows ships, when there is none (or
 * when AETHER_D3D12_ADAPTER=warp is set, to compare the two on one machine).
 *
 * Shaders are HLSL, entry point `main`: source text compiled at runtime
 * (vs_5_1 / ps_5_1 / cs_5_1), or bytecode compiled ahead of time (a DXBC
 * container, which is what fxc and dxc both produce). Resources map to
 * registers by binding number: a uniform at binding N is cbuffer bN, a
 * texture tN with its sampler sN, a storage buffer uN (all space0); the push
 * constants are cbuffer b0 in space1. Vertex attribute at location N is the
 * input semantic TEXCOORDN.
 *
 * Coordinates are Direct3D's: x and y run -1..1 with y pointing UP (Vulkan's
 * points down), and depth runs 0..1.
 *
 * Every entry point returns a status, never aborts, and is safe to call with
 * NULL. Errors carry text through aedx_last_error(). One device may be used
 * from several threads; a target, like Vulkan's, is drawn from one at a time.
 */

#ifndef AETHER_D3D12_H
#define AETHER_D3D12_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AEDX_OK                 0
#define AEDX_ERR_NO_LOADER     -1   /* no Direct3D 12 runtime on the system */
#define AEDX_ERR_NO_DEVICE     -2   /* runtime present, no usable adapter   */
#define AEDX_ERR_ARG           -3   /* caller passed something impossible   */
#define AEDX_ERR_OOM           -4   /* host or device allocation failed     */
#define AEDX_ERR_UNSUPPORTED   -5   /* device cannot do what was asked      */
#define AEDX_ERR_SHADER        -6   /* HLSL rejected or bytecode invalid    */
#define AEDX_ERR_DEVICE_LOST   -7   /* the device was removed or hung       */

/* Window kinds, numbered as aether-ui's native_view_kind() numbers them.
 * Direct3D 12 presents to a Win32 window only. */
#define AEDX_WINDOW_WIN32 1

typedef struct AedxDevice    AedxDevice;
typedef struct AedxTarget    AedxTarget;
typedef struct AedxPipeline  AedxPipeline;
typedef struct AedxLayout    AedxLayout;
typedef struct AedxBindings  AedxBindings;
typedef struct AedxTexture   AedxTexture;
typedef struct AedxMaterial  AedxMaterial;
typedef struct AedxBuffer    AedxBuffer;
typedef struct AedxCompute   AedxCompute;
typedef struct AedxSwapchain AedxSwapchain;

/* --- discovery ------------------------------------------------------------ */

int         aedx_available(void);
const char* aedx_last_error(void);
/* The adapter a fresh device would use; "" when none. */
const char* aedx_device_name(void);

/* --- device --------------------------------------------------------------- */

/* Picks the adapter (hardware first, WARP otherwise), creates the device, a
 * direct command queue and the shader-visible descriptor heaps. With
 * AETHER_D3D12_DEBUG=1 in the environment the debug layer is enabled and its
 * messages are printed to stderr; =2 adds GPU-based validation. */
AedxDevice* aedx_device_create(void);
void        aedx_device_destroy(AedxDevice* dev);
/* 1 when the device is WARP rather than a hardware adapter. */
int         aedx_device_is_warp(const AedxDevice* dev);
/* Warnings, errors and corruption the debug layer reported so far (0
 * without AETHER_D3D12_DEBUG); every message, INFO included, is printed. */
int         aedx_debug_message_count(const AedxDevice* dev);

/* --- targets -------------------------------------------------------------- */

AedxTarget* aedx_target_create(AedxDevice* dev, int width, int height);
AedxTarget* aedx_target_create_ex(AedxDevice* dev, int width, int height,
                                  int want_depth, int samples);
/* `format` is a DXGI_FORMAT: R8G8B8A8_UNORM (28), R8G8B8A8_UNORM_SRGB (29),
 * R16G16B16A16_FLOAT (10) or R32G32B32A32_FLOAT (2). */
AedxTarget* aedx_target_create_format(AedxDevice* dev, int width, int height, int format,
                                      int want_depth, int samples);
void        aedx_target_destroy(AedxTarget* t);
int         aedx_target_width(const AedxTarget* t);
int         aedx_target_height(const AedxTarget* t);
int         aedx_target_format(const AedxTarget* t);
int         aedx_target_bytes_per_pixel(const AedxTarget* t);
int         aedx_target_has_depth(const AedxTarget* t);
int         aedx_target_samples(const AedxTarget* t);
int         aedx_target_resize(AedxTarget* t, int width, int height);
int         aedx_target_set_readback(AedxTarget* t, int on);
int         aedx_target_readback(const AedxTarget* t);
int         aedx_target_set_frames(AedxTarget* t, int count);
int         aedx_target_frames(const AedxTarget* t);
int         aedx_target_set_timeout_ms(AedxTarget* t, int ms);
/* Bytes a full readback in the target's format holds: w * h * bpp. */
size_t      aedx_rgba_size(const AedxTarget* t);

/* --- vertex layouts, bindings, pipelines ----------------------------------- */

AedxLayout* aedx_layout_create(void);
void        aedx_layout_destroy(AedxLayout* l);
int         aedx_layout_binding(AedxLayout* l, int binding, int stride, int per_instance);
/* `format` is a DXGI_FORMAT (R32_FLOAT 41, R32G32_FLOAT 16, R32G32B32_FLOAT 6,
 * R32G32B32A32_FLOAT 2, R8G8B8A8_UNORM 28). */
int         aedx_layout_attr(AedxLayout* l, int location, int binding, int format, int offset);

AedxBindings* aedx_bindings_create(void);
void          aedx_bindings_destroy(AedxBindings* b);
int           aedx_bindings_uniform(AedxBindings* b, int binding);
int           aedx_bindings_texture(AedxBindings* b, int binding);
int           aedx_bindings_storage(AedxBindings* b, int binding);

/* A graphics pipeline for `target`'s format, sample count and depth.
 * `vs` / `ps` are HLSL source or DXBC bytecode, `vs_len` / `ps_len` their
 * sizes in bytes. With no layout the input is the built-in one: float2 at
 * TEXCOORD0 and float3 at TEXCOORD1, 20 bytes a vertex. An empty layout is a
 * pipeline with no vertex input. */
AedxPipeline* aedx_pipeline_create(AedxDevice* dev, AedxTarget* target,
                                   const void* vs, size_t vs_len,
                                   const void* ps, size_t ps_len);
AedxPipeline* aedx_pipeline_create_ex(AedxDevice* dev, AedxTarget* target,
                                      const void* vs, size_t vs_len,
                                      const void* ps, size_t ps_len,
                                      const AedxLayout* layout, int push_bytes,
                                      const AedxBindings* bindings);
void          aedx_pipeline_destroy(AedxPipeline* p);

/* --- textures and materials ------------------------------------------------ */

AedxTexture* aedx_texture_create(AedxDevice* dev, int width, int height);
AedxTexture* aedx_texture_create_ex(AedxDevice* dev, int width, int height,
                                    int mipmapped, int linear_filter, int repeat);
void         aedx_texture_destroy(AedxTexture* tex);
int          aedx_texture_mip_levels(const AedxTexture* tex);
/* A 3D texture of `depth` slices of width x height RGBA, read through a
 * Texture3D (#2198). No mip chain; upload is width*height*depth*4 bytes,
 * slice after slice. Slices: 1 for a 2D texture. */
AedxTexture* aedx_texture_create_3d(AedxDevice* dev, int width, int height, int depth,
                                    int linear_filter, int repeat);
int          aedx_texture_depth(const AedxTexture* tex);
int          aedx_texture_upload(AedxTexture* tex, const void* rgba, size_t len);

AedxMaterial* aedx_material_create(AedxPipeline* p);
void          aedx_material_destroy(AedxMaterial* m);
int           aedx_material_set_uniform(AedxMaterial* m, int binding, const void* data, size_t len);
int           aedx_material_set_texture(AedxMaterial* m, int binding, AedxTexture* tex);
/* A target's newest frame where a texture goes (#2198): its colour (depth 0)
 * or its depth as an R32_FLOAT texture (depth 1). Draw the target first. A
 * target cannot sample itself, one with no frame yet cannot be sampled, and
 * neither can a multisampled target's depth. Binding the depth the first
 * time rebuilds the target's images with a shader view of it. */
int           aedx_material_set_target(AedxMaterial* m, int binding, AedxTarget* t, int depth);
int           aedx_material_set_buffer(AedxMaterial* m, int binding, AedxBuffer* buf);

/* --- geometry, drawing, readback -------------------------------------------- */

int aedx_target_set_push(AedxTarget* t, const void* data, size_t len);
int aedx_batch_reset(AedxTarget* t);
int aedx_batch_add(AedxTarget* t, AedxMaterial* mat, int first, int count);

/* Instancing, indirect draws, dynamic uniform offsets and GPU timing, as in
 * contrib/vulkan (#2198). A dynamic uniform is a root CBV whose address
 * moves per draw, so offsets are multiples of 256. */
int    aedx_target_set_stream(AedxTarget* t, int binding, AedxBuffer* buf);
int    aedx_target_set_instances(AedxTarget* t, int count);
int    aedx_target_instances(const AedxTarget* t);
int    aedx_batch_add_instanced(AedxTarget* t, AedxMaterial* mat, int first, int count,
                                int first_instance, int instances);
int    aedx_batch_add_indirect(AedxTarget* t, AedxMaterial* mat, AedxBuffer* buf, int offset, int draws);
int    aedx_batch_set_offset(AedxTarget* t, int item, int binding, int offset);
int    aedx_uniform_offset_alignment(const AedxDevice* d);
int    aedx_bindings_uniform_dynamic(AedxBindings* b, int binding, int bytes);
int    aedx_target_set_timing(AedxTarget* t, int on);
double aedx_target_gpu_ms(const AedxTarget* t);
int    aedx_compute_set_timing(AedxCompute* c, int on);
double aedx_compute_gpu_ms(const AedxCompute* c);

/* A target's depth through a comparison sampler, for shadow maps (#2373):
 * `op` is 1 LESS, 2 LESS_EQUAL, 3 GREATER or 4 GREATER_EQUAL. */
int    aedx_pipeline_set_target_depth_compare(AedxPipeline* p, int binding, AedxTarget* t, int op);
int    aedx_material_set_target_depth_compare(AedxMaterial* m, int binding, AedxTarget* t, int op);
int    aedx_target_depth_linear(const AedxTarget* t);

/* Cube maps, 2D arrays and storage textures (#2387, #2388), as in
 * contrib/vulkan: TextureCube, Texture2DArray, and RWTexture2D/3D at uN in a
 * compute pass. */
AedxTexture* aedx_texture_create_cube(AedxDevice* d, int size, int mipmapped, int linear_filter);
AedxTexture* aedx_texture_create_array(AedxDevice* d, int w, int h, int layers, int mipmapped,
                                       int linear_filter, int repeat);
AedxTexture* aedx_texture_create_storage(AedxDevice* d, int w, int h, int depth, int format);
int          aedx_texture_layers(const AedxTexture* tex);

/* Stencil, and rendering into a layer (#2399), as in contrib.vulkan: depth 2
 * (DEPTH_STENCIL) is D24_UNORM_S8_UINT; a layered target is a texture array
 * (a cube's six slices) with a render target view a slice. */
AedxTarget*  aedx_target_create_layered(AedxDevice* d, int width, int height, int layers, int format,
                                        int want_depth, int cube);
int          aedx_target_set_layer(AedxTarget* t, int layer);
int          aedx_target_layers(const AedxTarget* t);
int          aedx_batch_set_stencil_ref(AedxTarget* t, int item, int ref);

/* Sampled textures in other formats, and anisotropic filtering (#2397), as
 * in contrib/vulkan, with DXGI_FORMAT values. A mipmapped uncompressed
 * chain is built on the CPU per format (sRGB averaged as linear light). */
AedxTexture* aedx_texture_create_format(AedxDevice* d, int w, int h, int format, int mipmapped,
                                        int linear_filter, int repeat, int anisotropy);
int          aedx_texture_format_supported(AedxDevice* d, int format);
int          aedx_texture_anisotropy(const AedxTexture* tex);
int          aedx_texture_upload_level(AedxTexture* tex, int level, const void* data, size_t len);
/* Cubes and arrays in any sampled format (#2402). */
AedxTexture* aedx_texture_create_cube_format(AedxDevice* d, int size, int format, int mipmapped,
                                             int linear_filter);
AedxTexture* aedx_texture_create_array_format(AedxDevice* d, int w, int h, int layers, int format,
                                              int mipmapped, int linear_filter, int repeat);

/* Targets with several colour attachments (#2386), as in contrib/vulkan. */
AedxTarget* aedx_target_create_mrt(AedxDevice* d, int width, int height, int count, int f0, int f1,
                                   int f2, int f3, int want_depth, int samples);
int         aedx_target_attachments(const AedxTarget* t);
int         aedx_pipeline_set_target_attachment(AedxPipeline* p, int binding, AedxTarget* t, int n);
int         aedx_material_set_target_attachment(AedxMaterial* m, int binding, AedxTarget* t, int n);
double      aedx_ae_pixel_value_at(void* t, int n, int x, int y, int channel);
int          aedx_bindings_storage_texture(AedxBindings* b, int binding);
int          aedx_compute_set_storage_texture(AedxCompute* c, int binding, AedxTexture* tex);

/* Pipeline state (#2385): blending (0 none, 1 alpha, 2 premultiplied,
 * 3 additive), culling (0 none, 1 back, 2 front; front faces run
 * counter-clockwise on screen) and the depth test (a compare op 1..4, and
 * whether it writes). NULL is the default every pipeline has. */
typedef struct AedxState AedxState;
AedxState* aedx_state_create(void);
void       aedx_state_destroy(AedxState* s);
int        aedx_state_blend(AedxState* s, int mode);
int        aedx_state_cull(AedxState* s, int mode);
int        aedx_state_depth(AedxState* s, int op, int write);
int        aedx_state_topology(AedxState* s, int topology);
/* The stencil test (#2399): COMPARE_* and STENCIL_* (Vulkan's numbering). */
int        aedx_state_stencil(AedxState* s, int compare, int ref, int pass_op, int fail_op,
                              int depth_fail_op, int read_mask, int write_mask);
/* A batch entry's own pipeline, made for the same target, and the channels
 * a pipeline writes (#2411). */
int        aedx_batch_set_pipeline(AedxTarget* t, int item, AedxPipeline* p);
int        aedx_state_color_mask(AedxState* s, int mask);
/* A layered target's depth as an array or a cube, raw (op 0) or compared,
 * and a multisampled layered target (#2412). */
int        aedx_pipeline_set_target_depth_array(AedxPipeline* p, int binding, AedxTarget* t, int op);
int        aedx_pipeline_set_target_depth_cube(AedxPipeline* p, int binding, AedxTarget* t, int op);
int        aedx_material_set_target_depth_array(AedxMaterial* m, int binding, AedxTarget* t, int op);
int        aedx_material_set_target_depth_cube(AedxMaterial* m, int binding, AedxTarget* t, int op);
AedxTarget* aedx_target_create_layered_ex(AedxDevice* d, int width, int height, int layers, int format,
                                          int want_depth, int cube, int samples);
int        aedx_pipeline_set_target_array(AedxPipeline* p, int binding, AedxTarget* t);
int        aedx_pipeline_set_target_cube(AedxPipeline* p, int binding, AedxTarget* t);
int        aedx_material_set_target_array(AedxMaterial* m, int binding, AedxTarget* t);
int        aedx_material_set_target_cube(AedxMaterial* m, int binding, AedxTarget* t);
int        aedx_batch_set_scissor(AedxTarget* t, int item, int x, int y, int w, int h);
int        aedx_batch_set_viewport(AedxTarget* t, int item, float x, float y, float w, float h,
                                  float min_depth, float max_depth);
AedxPipeline* aedx_pipeline_create_state(AedxDevice* d, AedxTarget* t, const void* vs, size_t vs_len,
                                         const void* ps, size_t ps_len, const AedxLayout* layout,
                                         int push_bytes, const AedxBindings* bindings,
                                         const AedxState* state);
int aedx_batch_count(const AedxTarget* t);
int aedx_draw(AedxTarget* t, AedxPipeline* p, AedxMaterial* mat,
              float r, float g, float b, float a);
int aedx_submit(AedxTarget* t, AedxPipeline* p, AedxMaterial* mat,
                float r, float g, float b, float a);
int aedx_wait_all(AedxTarget* t);
int aedx_read_rgba(AedxTarget* t, void* out, size_t out_len);
int aedx_read_rgba8(AedxTarget* t, void* out, size_t out_len);

/* --- buffers and compute ----------------------------------------------------- */

AedxBuffer*  aedx_buffer_create(AedxDevice* dev, size_t bytes);
void         aedx_buffer_destroy(AedxBuffer* b);
size_t       aedx_buffer_size(const AedxBuffer* b);
int          aedx_buffer_write(AedxBuffer* b, size_t offset, const void* data, size_t len);
int          aedx_buffer_read(AedxBuffer* b, size_t offset, void* out, size_t len);

AedxCompute* aedx_compute_create(AedxDevice* dev, const void* cs, size_t len,
                                 const AedxBindings* bindings, int push_bytes);
void         aedx_compute_destroy(AedxCompute* c);
int          aedx_compute_set_buffer(AedxCompute* c, int binding, AedxBuffer* buf);
int          aedx_compute_set_texture(AedxCompute* c, int binding, AedxTexture* tex);
int          aedx_compute_set_push(AedxCompute* c, const void* data, size_t len);
int          aedx_compute_set_timeout_ms(AedxCompute* c, int ms);
int          aedx_dispatch(AedxCompute* c, int gx, int gy, int gz);
int          aedx_dispatch_async(AedxCompute* c, int gx, int gy, int gz);
int          aedx_compute_wait(AedxCompute* c);

/* --- presentation ------------------------------------------------------------- */

/* A flip-model swapchain on a Win32 window (kind AEDX_WINDOW_WIN32, `window`
 * an HWND) the caller owns. The size follows the window's client area. */
AedxSwapchain* aedx_swapchain_create(AedxDevice* dev, int kind, void* display,
                                     void* window, int width, int height);
void           aedx_swapchain_destroy(AedxSwapchain* sc);
int            aedx_swapchain_resize(AedxSwapchain* sc, int width, int height);
int            aedx_swapchain_set_vsync(AedxSwapchain* sc, int on);
int            aedx_swapchain_width(const AedxSwapchain* sc);
int            aedx_swapchain_height(const AedxSwapchain* sc);
int            aedx_swapchain_format(const AedxSwapchain* sc);
long long      aedx_swapchain_presented(const AedxSwapchain* sc);
/* Draws the target's newest frame into the next back buffer (scaled, and
 * encoded for display when the target is sRGB or float) and presents it. */
int            aedx_present(AedxSwapchain* sc, AedxTarget* t);

#ifdef __cplusplus
}
#endif

#endif /* AETHER_D3D12_H */
