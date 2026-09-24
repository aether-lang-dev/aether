/* contrib/metal: GPU rendering, compute and presentation with Metal.
 *
 * The same shape as contrib/vulkan and contrib/d3d12: a device, offscreen
 * targets, pipelines with vertex layouts, bindings, textures and materials,
 * batches, frames in flight, colour formats, readback, compute, and
 * swapchains over a view or layer someone else owns. A program that knows one
 * knows the others; what differs is the shading language and the coordinate
 * conventions (below).
 *
 * Metal, QuartzCore, Foundation and the Objective-C runtime are opened at
 * RUNTIME, so nothing links against them: a program using this module builds
 * on every platform, and aemt_available() reports 0 where there is no Metal
 * device (any system other than macOS). The device is the system default one,
 * MTLCreateSystemDefaultDevice().
 *
 * Shaders are Metal Shading Language: source compiled at runtime, or a
 * metallib compiled ahead of time (`xcrun metal` + `xcrun metallib`). The
 * entry point is the library's one function of the stage's kind (vertex,
 * fragment, kernel), whatever its name. Resources map to argument-table
 * indices by binding number: a uniform or a storage buffer at binding N is
 * [[buffer(N)]], a texture [[texture(N)]] with its sampler [[sampler(N)]];
 * the push constants are [[buffer(8)]], and vertex stream B is buffer 16 + B.
 * Vertex attribute at location N is [[attribute(N)]] of the [[stage_in]]
 * struct. MSL does not declare a compute kernel's threadgroup size, so the
 * caller gives it with aemt_compute_set_group_size.
 *
 * Formats are named, and numbered, as contrib.vulkan names them: Metal keeps
 * pixel formats and vertex formats in two enums, and one constant here serves
 * both (FORMAT_R32G32B32A32_SFLOAT is a target format and a vertex attribute
 * format), so the module translates the number where it is used.
 *
 * Coordinates are Metal's: x and y run -1..1 with y pointing UP (Vulkan's
 * points down), and depth runs 0..1, as in Direct3D.
 *
 * Every entry point returns a status, never aborts, and is safe to call with
 * NULL. Errors carry text through aemt_last_error(). One device may be used
 * from several threads; a target is drawn from one at a time.
 */

#ifndef AETHER_METAL_H
#define AETHER_METAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AEMT_OK                 0
#define AEMT_ERR_NO_LOADER     -1   /* no Metal on the system                */
#define AEMT_ERR_NO_DEVICE     -2   /* Metal present, no device              */
#define AEMT_ERR_ARG           -3   /* caller passed something impossible    */
#define AEMT_ERR_OOM           -4   /* host or device allocation failed      */
#define AEMT_ERR_UNSUPPORTED   -5   /* device cannot do what was asked       */
#define AEMT_ERR_SHADER        -6   /* MSL rejected or metallib invalid      */
#define AEMT_ERR_DEVICE_LOST   -7   /* a command buffer failed or timed out  */

/* Window kinds, numbered as aether-ui's native_view_kind() numbers them.
 * Metal presents through a CAMetalLayer: an NSView is given one, or the
 * caller hands over its own. */
#define AEMT_WINDOW_NSVIEW      2
#define AEMT_WINDOW_METAL_LAYER 5

/* Formats, numbered as contrib.vulkan numbers them (VkFormat). */
#define AEMT_FORMAT_R8G8B8A8_UNORM      37
#define AEMT_FORMAT_R8G8B8A8_SRGB       43
#define AEMT_FORMAT_B8G8R8A8_UNORM      44
#define AEMT_FORMAT_B8G8R8A8_SRGB       50
#define AEMT_FORMAT_R16G16B16A16_SFLOAT 97
#define AEMT_FORMAT_R32_SFLOAT          100
#define AEMT_FORMAT_R32G32_SFLOAT       103
#define AEMT_FORMAT_R32G32B32_SFLOAT    106
#define AEMT_FORMAT_R32G32B32A32_SFLOAT 109

typedef struct AemtDevice    AemtDevice;
typedef struct AemtTarget    AemtTarget;
typedef struct AemtPipeline  AemtPipeline;
typedef struct AemtLayout    AemtLayout;
typedef struct AemtBindings  AemtBindings;
typedef struct AemtTexture   AemtTexture;
typedef struct AemtMaterial  AemtMaterial;
typedef struct AemtBuffer    AemtBuffer;
typedef struct AemtCompute   AemtCompute;
typedef struct AemtSwapchain AemtSwapchain;

/* --- discovery ------------------------------------------------------------ */

int         aemt_available(void);
const char* aemt_last_error(void);
/* The device a fresh aemt_device_create would use; "" when none. */
const char* aemt_device_name(void);

/* --- device --------------------------------------------------------------- */

AemtDevice* aemt_device_create(void);
void        aemt_device_destroy(AemtDevice* dev);
/* 1 when the device shares memory with the CPU (Apple silicon). */
int         aemt_device_unified_memory(const AemtDevice* dev);

/* --- targets -------------------------------------------------------------- */

AemtTarget* aemt_target_create(AemtDevice* dev, int width, int height);
AemtTarget* aemt_target_create_ex(AemtDevice* dev, int width, int height,
                                  int want_depth, int samples);
/* `format`: AEMT_FORMAT_R8G8B8A8_UNORM, _R8G8B8A8_SRGB, _R16G16B16A16_SFLOAT
 * or _R32G32B32A32_SFLOAT. */
AemtTarget* aemt_target_create_format(AemtDevice* dev, int width, int height, int format,
                                      int want_depth, int samples);
void        aemt_target_destroy(AemtTarget* t);
int         aemt_target_width(const AemtTarget* t);
int         aemt_target_height(const AemtTarget* t);
int         aemt_target_format(const AemtTarget* t);
int         aemt_target_bytes_per_pixel(const AemtTarget* t);
int         aemt_target_has_depth(const AemtTarget* t);
int         aemt_target_samples(const AemtTarget* t);
int         aemt_target_resize(AemtTarget* t, int width, int height);
int         aemt_target_set_readback(AemtTarget* t, int on);
int         aemt_target_readback(const AemtTarget* t);
int         aemt_target_set_frames(AemtTarget* t, int count);
int         aemt_target_frames(const AemtTarget* t);
int         aemt_target_set_timeout_ms(AemtTarget* t, int ms);
/* Bytes a full readback in the target's format holds: w * h * bpp. */
size_t      aemt_rgba_size(const AemtTarget* t);

/* --- vertex layouts, bindings, pipelines ----------------------------------- */

AemtLayout* aemt_layout_create(void);
void        aemt_layout_destroy(AemtLayout* l);
int         aemt_layout_binding(AemtLayout* l, int binding, int stride, int per_instance);
/* `format`: AEMT_FORMAT_R32_SFLOAT, _R32G32_SFLOAT, _R32G32B32_SFLOAT,
 * _R32G32B32A32_SFLOAT, or _R8G8B8A8_UNORM (four normalised bytes). */
int         aemt_layout_attr(AemtLayout* l, int location, int binding, int format, int offset);

AemtBindings* aemt_bindings_create(void);
void          aemt_bindings_destroy(AemtBindings* b);
int           aemt_bindings_uniform(AemtBindings* b, int binding);
int           aemt_bindings_texture(AemtBindings* b, int binding);
int           aemt_bindings_storage(AemtBindings* b, int binding);

/* A render pipeline for `target`'s format, sample count and depth. `vs` /
 * `fs` are MSL source or a metallib, `vs_len` / `fs_len` their sizes in
 * bytes. With no layout the input is the built-in one: float2 at
 * [[attribute(0)]] and float3 at [[attribute(1)]], 20 bytes a vertex. An empty
 * layout is a pipeline with no vertex input. */
AemtPipeline* aemt_pipeline_create(AemtDevice* dev, AemtTarget* target,
                                   const void* vs, size_t vs_len,
                                   const void* fs, size_t fs_len);
AemtPipeline* aemt_pipeline_create_ex(AemtDevice* dev, AemtTarget* target,
                                      const void* vs, size_t vs_len,
                                      const void* fs, size_t fs_len,
                                      const AemtLayout* layout, int push_bytes,
                                      const AemtBindings* bindings);
void          aemt_pipeline_destroy(AemtPipeline* p);

/* --- textures and materials ------------------------------------------------ */

AemtTexture* aemt_texture_create(AemtDevice* dev, int width, int height);
AemtTexture* aemt_texture_create_ex(AemtDevice* dev, int width, int height,
                                    int mipmapped, int linear_filter, int repeat);
void         aemt_texture_destroy(AemtTexture* tex);
int          aemt_texture_mip_levels(const AemtTexture* tex);
int          aemt_texture_upload(AemtTexture* tex, const void* rgba, size_t len);

AemtMaterial* aemt_material_create(AemtPipeline* p);
void          aemt_material_destroy(AemtMaterial* m);
int           aemt_material_set_uniform(AemtMaterial* m, int binding, const void* data, size_t len);
int           aemt_material_set_texture(AemtMaterial* m, int binding, AemtTexture* tex);
int           aemt_material_set_buffer(AemtMaterial* m, int binding, AemtBuffer* buf);

/* --- geometry, drawing, readback -------------------------------------------- */

int aemt_target_set_push(AemtTarget* t, const void* data, size_t len);
int aemt_batch_reset(AemtTarget* t);
int aemt_batch_add(AemtTarget* t, AemtMaterial* mat, int first, int count);
int aemt_batch_count(const AemtTarget* t);
int aemt_draw(AemtTarget* t, AemtPipeline* p, AemtMaterial* mat,
              float r, float g, float b, float a);
int aemt_submit(AemtTarget* t, AemtPipeline* p, AemtMaterial* mat,
                float r, float g, float b, float a);
int aemt_wait_all(AemtTarget* t);
int aemt_read_rgba(AemtTarget* t, void* out, size_t out_len);
int aemt_read_rgba8(AemtTarget* t, void* out, size_t out_len);

/* --- buffers and compute ----------------------------------------------------- */

AemtBuffer*  aemt_buffer_create(AemtDevice* dev, size_t bytes);
void         aemt_buffer_destroy(AemtBuffer* b);
size_t       aemt_buffer_size(const AemtBuffer* b);
int          aemt_buffer_write(AemtBuffer* b, size_t offset, const void* data, size_t len);
int          aemt_buffer_read(AemtBuffer* b, size_t offset, void* out, size_t len);

AemtCompute* aemt_compute_create(AemtDevice* dev, const void* cs, size_t len,
                                 const AemtBindings* bindings, int push_bytes);
void         aemt_compute_destroy(AemtCompute* c);
/* Threads a group, which HLSL's [numthreads] and GLSL's local_size give in the
 * shader and MSL does not: required before the first dispatch. */
int          aemt_compute_set_group_size(AemtCompute* c, int x, int y, int z);
int          aemt_compute_set_buffer(AemtCompute* c, int binding, AemtBuffer* buf);
int          aemt_compute_set_texture(AemtCompute* c, int binding, AemtTexture* tex);
int          aemt_compute_set_push(AemtCompute* c, const void* data, size_t len);
int          aemt_compute_set_timeout_ms(AemtCompute* c, int ms);
int          aemt_dispatch(AemtCompute* c, int gx, int gy, int gz);
int          aemt_dispatch_async(AemtCompute* c, int gx, int gy, int gz);
int          aemt_compute_wait(AemtCompute* c);

/* --- presentation ------------------------------------------------------------- */

/* A swapchain over an NSView (kind AEMT_WINDOW_NSVIEW, given a CAMetalLayer on
 * the main thread) or a CAMetalLayer (AEMT_WINDOW_METAL_LAYER) the caller
 * owns. A view's size is followed; a bare layer takes width x height. */
AemtSwapchain* aemt_swapchain_create(AemtDevice* dev, int kind, void* display,
                                     void* window, int width, int height);
void           aemt_swapchain_destroy(AemtSwapchain* sc);
int            aemt_swapchain_resize(AemtSwapchain* sc, int width, int height);
int            aemt_swapchain_set_vsync(AemtSwapchain* sc, int on);
int            aemt_swapchain_width(const AemtSwapchain* sc);
int            aemt_swapchain_height(const AemtSwapchain* sc);
int            aemt_swapchain_format(const AemtSwapchain* sc);
long long      aemt_swapchain_presented(const AemtSwapchain* sc);
/* Draws the target's newest frame into the next drawable (scaled, and encoded
 * for display when the target is sRGB or float) and presents it. */
int            aemt_present(AemtSwapchain* sc, AemtTarget* t);

#ifdef __cplusplus
}
#endif

#endif /* AETHER_METAL_H */
