/* contrib/vulkan: offscreen GPU rendering for Aether (#1495, phase 1).
 *
 * Loads the Vulkan loader at RUNTIME. Nothing here links against libvulkan, so
 * this file compiles and the binary starts on a machine with no driver at all;
 * aevk_available() then reports 0 and the caller degrades. Only the Vulkan
 * HEADERS are needed to build, and they are header-only.
 *
 * Every entry point returns a status, never aborts, and is safe to call with
 * NULL. Errors carry text through aevk_last_error().
 */

#ifndef AETHER_VULKAN_H
#define AETHER_VULKAN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes. Negative values are this layer's own; a positive value is a
 * VkResult passed through unchanged so callers can report the driver's answer. */
#define AEVK_OK                 0
#define AEVK_ERR_NO_LOADER     -1   /* no libvulkan on the system            */
#define AEVK_ERR_NO_DEVICE     -2   /* loader present, no usable GPU         */
#define AEVK_ERR_ARG           -3   /* caller passed something impossible    */
#define AEVK_ERR_OOM           -4   /* host or device allocation failed      */
#define AEVK_ERR_UNSUPPORTED   -5   /* device cannot do what was asked       */
#define AEVK_ERR_SHADER        -6   /* SPIR-V rejected                       */
#define AEVK_ERR_DEVICE_LOST   -7   /* driver reported the device is gone    */

typedef struct AevkDevice AevkDevice;
typedef struct AevkTarget AevkTarget;
typedef struct AevkPipeline AevkPipeline;
typedef struct AevkLayout AevkLayout;
typedef struct AevkBindings AevkBindings;
typedef struct AevkTexture AevkTexture;
typedef struct AevkMaterial AevkMaterial;
typedef struct AevkSwapchain AevkSwapchain;
typedef struct AevkBuffer AevkBuffer;
typedef struct AevkCompute AevkCompute;

/* The kinds of window a swapchain can present into (#1505), numbered as
 * aether-ui's native_view_kind() numbers them so a toolkit's handle passes
 * straight through. 5 is for a program that made its own CAMetalLayer, as a
 * GLFW-based one does on macOS. */
#define AEVK_WINDOW_WIN32        1   /* window = HWND                         */
#define AEVK_WINDOW_NSVIEW       2   /* window = NSView*; given a CAMetalLayer */
#define AEVK_WINDOW_X11          3   /* display = Display*, window = Window    */
#define AEVK_WINDOW_WAYLAND      4   /* display = wl_display*, window = wl_surface* */
#define AEVK_WINDOW_METAL_LAYER  5   /* window = CAMetalLayer*                 */

/* 1 when a loader AND at least one physical device are present. Cheap after
 * the first call: the probe result is cached, including the negative. */
int aevk_available(void);

/* Human-readable text for the most recent failure on the calling thread.
 * Never NULL; "" when nothing has failed. */
const char* aevk_last_error(void);

/* Name of the physical device a fresh aevk_device_create would select, or ""
 * when none is usable. For diagnostics and tests. */
const char* aevk_device_name(void);

/* --- device ------------------------------------------------------------- */

/* Creates the instance, picks a physical device (discrete GPU first, then
 * integrated, then anything with a graphics queue), and creates the logical
 * device, queue and command pool. Returns NULL on failure. */
AevkDevice* aevk_device_create(void);
void        aevk_device_destroy(AevkDevice* dev);

/* --- offscreen target ---------------------------------------------------- */

/* A colour image of `width` x `height` in R8G8B8A8_UNORM, its view, a
 * render pass, a framebuffer, and a host-visible readback buffer that stays
 * mapped for the target's lifetime. Fails with AEVK_ERR_UNSUPPORTED rather
 * than crashing when the size exceeds the device's limits. */
AevkTarget* aevk_target_create(AevkDevice* dev, int width, int height);

/* As above, with an optional depth attachment and multisampling (#1512).
 *
 * `want_depth` adds a depth attachment in a format the device supports, and
 * pipelines built for this target then test and write depth, so overlapping
 * geometry resolves by distance instead of by submission order.
 *
 * `samples` is 1, 2, 4, 8 or 16, checked against what the device actually
 * offers for framebuffers rather than rounded down silently. Above 1 the
 * colour attachment is multisampled and resolves into the single-sample image,
 * so readback and `pixel()` are unchanged. */
AevkTarget* aevk_target_create_ex(AevkDevice* dev, int width, int height,
                                  int want_depth, int samples);

/* New images and framebuffer at `width` x `height`, keeping the render pass,
 * so every pipeline made for the target stays valid: the call for a window
 * that changed size. Geometry, push constants, batch and frame count carry
 * over; the previous contents do not. */
int aevk_target_resize(AevkTarget* t, int width, int height);

/* Whether each frame is copied into host memory for readback (on by
 * default). A target that is only presented turns it off and stops paying a
 * width*height*4 copy and a readback buffer per frame slot; the readers then
 * refuse with AEVK_ERR_ARG. */
int aevk_target_set_readback(AevkTarget* t, int on);
int aevk_target_readback(const AevkTarget* t);

/* As target_create_ex, in a chosen colour format (#1514): a VkFormat value,
 * one of R8G8B8A8_UNORM (37, the default), R8G8B8A8_SRGB (43),
 * R16G16B16A16_SFLOAT (97) or R32G32B32A32_SFLOAT (109). Checked against the
 * device's attachment support and refused with AEVK_ERR_UNSUPPORTED when it
 * cannot render to it. The readback buffers hold the format's own bytes. */
AevkTarget* aevk_target_create_format(AevkDevice* dev, int width, int height, int format,
                                      int want_depth, int samples);

/* The target's VkFormat and its size in bytes a pixel (4, 8 or 16). */
int aevk_target_format(const AevkTarget* t);
int aevk_target_bytes_per_pixel(const AevkTarget* t);

/* 1 when the target has a depth attachment; its sample count (1 when not
 * multisampled). For tests and diagnostics. */
int aevk_target_has_depth(const AevkTarget* t);
int aevk_target_samples(const AevkTarget* t);
void        aevk_target_destroy(AevkTarget* t);
int         aevk_target_width(const AevkTarget* t);
int         aevk_target_height(const AevkTarget* t);

/* --- pipeline ------------------------------------------------------------ */

/* Graphics pipeline from SPIR-V. `vert_spv` / `frag_spv` are SPIR-V words;
 * sizes are in BYTES and must be a non-zero multiple of 4. Vertex input is
 * one interleaved binding: vec2 position at location 0, vec3 colour at
 * location 1, stride 20 bytes. */
AevkPipeline* aevk_pipeline_create(AevkDevice* dev, AevkTarget* target,
                                   const void* vert_spv, size_t vert_len,
                                   const void* frag_spv, size_t frag_len);

/* As above, plus a caller-described vertex layout, a push-constant block of
 * `push_bytes` (0 for none, a multiple of 4, at most 128), and declared
 * shader resources. Any of `layout` / `bindings` may be NULL. The pipeline
 * owns one descriptor set: enough for a transform, a material and its
 * textures, with a lifetime that cannot outlive its pool. */
AevkPipeline* aevk_pipeline_create_ex(AevkDevice* dev, AevkTarget* target,
                                      const void* vert_spv, size_t vert_len,
                                      const void* frag_spv, size_t frag_len,
                                      const AevkLayout* layout,
                                      int push_bytes,
                                      const AevkBindings* bindings);
void          aevk_pipeline_destroy(AevkPipeline* p);

/* --- vertex layout -------------------------------------------------------- */

/* Describes vertex input for pipeline_create_ex. Without one the pipeline
 * uses the built-in layout: one interleaved stream of vec2 position and vec3
 * colour, stride 20. An empty layout (nothing described) is a pipeline with
 * no vertex input, whose vertex shader pulls its data from a storage buffer
 * by gl_VertexIndex. `format` is a VkFormat value. */
AevkLayout* aevk_layout_create(void);
void        aevk_layout_destroy(AevkLayout* l);
int         aevk_layout_binding(AevkLayout* l, int binding, int stride, int per_instance);
int         aevk_layout_attr(AevkLayout* l, int location, int binding, int format, int offset);

/* --- shader resources ----------------------------------------------------- */

/* Declares what a shader reads besides vertex attributes. Bindings are
 * visible to both the vertex and fragment stage. */
AevkBindings* aevk_bindings_create(void);
void          aevk_bindings_destroy(AevkBindings* b);
int           aevk_bindings_uniform(AevkBindings* b, int binding);
int           aevk_bindings_texture(AevkBindings* b, int binding);

/* An R8G8B8A8_UNORM sampled image with a nearest-filter, clamped sampler.
 * A texture must be uploaded before it is bound: sampling an image that was
 * never given pixels is undefined, so binding one is refused. */
AevkTexture* aevk_texture_create(AevkDevice* dev, int width, int height);

/* As above, with a mip chain and sampler choices. `mipmapped` builds a full
 * chain on upload, so a minified texture stops aliasing; it needs the device
 * to support linear blitting of R8G8B8A8_UNORM and is refused otherwise rather
 * than producing an empty chain. `linear_filter` selects LINEAR over NEAREST
 * for magnification, minification and between levels; `repeat` selects REPEAT
 * over CLAMP_TO_EDGE. */
AevkTexture* aevk_texture_create_ex(AevkDevice* dev, int width, int height,
                                    int mipmapped, int linear_filter, int repeat);

/* Mip levels the texture carries: 1 when it was not built mipmapped. */
int aevk_texture_mip_levels(const AevkTexture* tex);

/* A 3D texture of `depth` slices, each width x height RGBA, read in a shader
 * through a sampler3D (#2198). No mip chain. Upload takes
 * width*height*depth*4 bytes, slice after slice; each side is limited by the
 * device's maxImageDimension3D. */
AevkTexture* aevk_texture_create_3d(AevkDevice* dev, int width, int height, int depth,
                                    int linear_filter, int repeat);
/* Slices: 1 for a 2D texture. */
int aevk_texture_depth(const AevkTexture* tex);

/* Cube maps and 2D arrays (#2387): six faces uploaded +X, -X, +Y, -Y, +Z,
 * -Z, or `layers` images one after another; mip chains are built per layer.
 * texture_layers is 6 for a cube, the layers of an array, 1 otherwise. */
AevkTexture* aevk_texture_create_cube(AevkDevice* d, int size, int mipmapped, int linear_filter);
AevkTexture* aevk_texture_create_array(AevkDevice* d, int width, int height, int layers,
                                       int mipmapped, int linear_filter, int repeat);
int          aevk_texture_layers(const AevkTexture* tex);

/* Sampled textures in other formats, and anisotropic filtering (#2397):
 * R8, RG8, RGBA8 UNORM and sRGB, RGBA16F, RGBA32F, and the BC1, BC3, BC4, BC5
 * and BC7 block formats (as VkFormat values). A block format's mip levels
 * are uploaded one by one with texture_upload_level; texture_upload is its
 * level 0. Anisotropy is granted up to the device's limit. */
AevkTexture* aevk_texture_create_format(AevkDevice* d, int width, int height, int format, int mipmapped,
                                        int linear_filter, int repeat, int anisotropy);
int          aevk_texture_format_supported(AevkDevice* d, int format);
int          aevk_texture_anisotropy(const AevkTexture* tex);
int          aevk_texture_upload_level(AevkTexture* tex, int level, const void* data, size_t len);

/* Textures compute passes write (#2388): 2D, or 3D when depth > 1, in
 * VK_FORMAT_R8G8B8A8_UNORM, R16G16B16A16_SFLOAT or R32G32B32A32_SFLOAT;
 * zeroed at creation, sampled by draws by nearest texel. A storage binding
 * is a compute pass's only. */
AevkTexture* aevk_texture_create_storage(AevkDevice* d, int width, int height, int depth, int format);
int          aevk_bindings_storage_texture(AevkBindings* b, int binding);
int          aevk_compute_set_storage_texture(AevkCompute* c, int binding, AevkTexture* tex);
void         aevk_texture_destroy(AevkTexture* tex);
int          aevk_texture_upload(AevkTexture* tex, const void* rgba, size_t len);

/* --- materials -------------------------------------------------------------- */

/* A descriptor set plus the uniform buffers written into it. Several per
 * pipeline, so one pipeline draws several objects with different textures and
 * constants in a frame rather than needing a pipeline per material, which
 * would duplicate the shader modules for nothing.
 *
 * Descriptor pools grow a block at a time, so there is no fixed ceiling on how
 * many a scene may have. Destroy materials before the pipeline that made them:
 * the set belongs to a pool the pipeline owns. */
AevkMaterial* aevk_material_create(AevkPipeline* p);
void          aevk_material_destroy(AevkMaterial* m);
int           aevk_material_set_uniform(AevkMaterial* m, int binding,
                                        const void* data, size_t len);
int           aevk_material_set_texture(AevkMaterial* m, int binding, AevkTexture* tex);

/* A target's newest frame where a texture goes (#2198): its colour, or its
 * depth (a single-channel float, near 0 to far 1), read in a later pass. The
 * frame read is the one most recently submitted to the target when the draw
 * runs on the queue, so draw the target first. Refused while that target is
 * the one being drawn, before it has a frame, and for the depth of a target
 * without one or of a multisampled one. Binding the depth the first time
 * switches the target to storing it, with the device idle. The target must
 * outlive every draw that uses the material. */
int           aevk_material_set_target_color(AevkMaterial* m, int binding, AevkTarget* t);
int           aevk_material_set_target_depth(AevkMaterial* m, int binding, AevkTarget* t);

/* Draw or submit with a specific material. Passing NULL uses the pipeline's
 * default, which is what `aevk_draw` and `aevk_submit` do. */
int aevk_draw_material(AevkTarget* t, AevkPipeline* p, AevkMaterial* mat,
                       float r, float g, float b, float a);
int aevk_submit_material(AevkTarget* t, AevkPipeline* p, AevkMaterial* mat,
                         float r, float g, float b, float a);

/* Several draws inside one frame, each with its own material. `first` and
 * `count` address indices when the target has an index buffer and vertices
 * otherwise. An empty batch, the default, draws all the geometry once.
 *
 * A material referenced by a batch must outlive the draws that use it:
 * destroying one without resetting the batch leaves a dangling reference,
 * the same contract Vulkan gives for a resource bound to a descriptor set. */
int aevk_batch_reset(AevkTarget* t);
int aevk_batch_add(AevkTarget* t, AevkMaterial* mat, int first, int count);
int aevk_batch_count(const AevkTarget* t);

/* --- instancing, indirect draws, dynamic offsets, timing (#2198) ----------- */

/* A caller's buffer as vertex stream `binding`, 1..7 (binding 0 is the
 * target's own vertices); NULL unbinds it. The pipeline's layout gives its
 * stride and rate. A draw refuses a stream the pipeline reads that is unbound
 * or too short for the vertices or instances drawn. */
int aevk_target_set_stream(AevkTarget* t, int binding, AevkBuffer* buf);
/* Instances each draw makes, 1 by default, unless a batch entry says. */
int aevk_target_set_instances(AevkTarget* t, int count);
int aevk_target_instances(const AevkTarget* t);
/* A batch entry drawing `instances` copies of the range from instance
 * `first_instance`. */
int aevk_batch_add_instanced(AevkTarget* t, AevkMaterial* mat, int first, int count,
                             int first_instance, int instances);
/* A batch entry whose `draws` commands are read from `buf` at `offset` when
 * the frame runs: five 32-bit words each with indices (index count, instance
 * count, first index, vertex offset, first instance), four without (vertex
 * count, instance count, first vertex, first instance). */
int aevk_batch_add_indirect(AevkTarget* t, AevkMaterial* mat, AevkBuffer* buf, int offset, int draws);
/* The offset batch entry `item` reads dynamic uniform `binding` at: a
 * multiple of aevk_uniform_offset_alignment. */
int aevk_batch_set_offset(AevkTarget* t, int item, int binding, int offset);
int aevk_uniform_offset_alignment(const AevkDevice* d);
/* A uniform read as a `bytes` window of the buffer set_buffer binds, at the
 * offset each draw chooses. Graphics pipelines only. */
int aevk_bindings_uniform_dynamic(AevkBindings* b, int binding, int bytes);
/* GPU time of a target's frames and a compute pass's dispatches, in
 * milliseconds, for the newest one waited for; -1 while timing is off or
 * before one has finished. ERR_UNSUPPORTED on a queue with no timestamps. */
int    aevk_target_set_timing(AevkTarget* t, int on);
double aevk_target_gpu_ms(const AevkTarget* t);
int    aevk_compute_set_timing(AevkCompute* c, int on);
double aevk_compute_gpu_ms(const AevkCompute* c);

/* Writes a uniform buffer, creating and binding it on first use, then
 * copying on every call. Host-coherent, so a per-frame update is a memcpy
 * and no descriptor rewrite. */
int aevk_pipeline_set_uniform(AevkPipeline* p, int binding, const void* data, size_t len);

/* Points a combined-image-sampler binding at `tex`. */
int aevk_pipeline_set_texture(AevkPipeline* p, int binding, AevkTexture* tex);

/* The same, at a target's colour (depth 0) or depth (depth 1). */
int aevk_pipeline_set_target(AevkPipeline* p, int binding, AevkTarget* t, int depth);

/* A target's depth through a comparison sampler, for shadow maps (#2373):
 * `op` is 1 LESS, 2 LESS_EQUAL, 3 GREATER or 4 GREATER_EQUAL, and the shader
 * (a sampler2DShadow) gets the fraction of the footprint whose depth passes
 * `reference op texel`. Filtered across texels where target_depth_linear. */
int aevk_pipeline_set_target_depth_compare(AevkPipeline* p, int binding, AevkTarget* t, int op);
int aevk_material_set_target_depth_compare(AevkMaterial* m, int binding, AevkTarget* t, int op);
int aevk_target_depth_linear(const AevkTarget* t);

/* Targets with several colour attachments (#2386): `count` 1..4, attachment
 * N in format fN, written by fragment output N and cleared to the draw's
 * colour. Present and the whole-frame readers use attachment 0. */
AevkTarget* aevk_target_create_mrt(AevkDevice* d, int width, int height, int count, int f0, int f1,
                                   int f2, int f3, int want_depth, int samples);
int         aevk_target_attachments(const AevkTarget* t);
int         aevk_pipeline_set_target_attachment(AevkPipeline* p, int binding, AevkTarget* t, int n);
int         aevk_material_set_target_attachment(AevkMaterial* m, int binding, AevkTarget* t, int n);

/* Pipeline state (#2385): blending (0 none, 1 alpha, 2 premultiplied,
 * 3 additive), culling (0 none, 1 back, 2 front; front faces run
 * counter-clockwise on screen) and the depth test (a compare op 1..4, and
 * whether it writes). NULL is the default every pipeline has: no blending,
 * no culling, a LESS test that writes. */
typedef struct AevkState AevkState;
AevkState* aevk_state_create(void);
void       aevk_state_destroy(AevkState* s);
int        aevk_state_blend(AevkState* s, int mode);
int        aevk_state_cull(AevkState* s, int mode);
int        aevk_state_depth(AevkState* s, int op, int write);
/* Topology (#2398): 0 triangles, 1 triangle strip, 2 lines, 3 line strip,
 * 4 points (the vertex shader writes gl_PointSize). */
int        aevk_state_topology(AevkState* s, int topology);

/* Stencil, and rendering into a layer (#2399). Depth 2 (DEPTH_STENCIL) on
 * any target create adds a stencil, cleared to 0 with the depth; the
 * pipeline state's stencil test uses COMPARE_* and STENCIL_* (VkStencilOp's
 * numbering), and a draw may carry its own reference. A layered target has
 * `layers` layers (a cube's six faces), rendered one at a time after
 * target_set_layer and sampled whole, as a sampler2DArray or samplerCube. */
int          aevk_state_stencil(AevkState* s, int compare, int ref, int pass_op, int fail_op,
                                int depth_fail_op, int read_mask, int write_mask);
int          aevk_batch_set_stencil_ref(AevkTarget* t, int item, int ref);
AevkTarget*  aevk_target_create_layered(AevkDevice* d, int width, int height, int layers, int format,
                                        int want_depth, int cube);
int          aevk_target_set_layer(AevkTarget* t, int layer);
int          aevk_target_layers(const AevkTarget* t);
int          aevk_pipeline_set_target_array(AevkPipeline* p, int binding, AevkTarget* t);
int          aevk_pipeline_set_target_cube(AevkPipeline* p, int binding, AevkTarget* t);
int          aevk_material_set_target_array(AevkMaterial* m, int binding, AevkTarget* t);
int          aevk_material_set_target_cube(AevkMaterial* m, int binding, AevkTarget* t);
/* A batch entry's scissor and viewport, in pixels from the target's top
 * left (#2398); a width of 0 goes back to the whole target. */
int        aevk_batch_set_scissor(AevkTarget* t, int item, int x, int y, int w, int h);
int        aevk_batch_set_viewport(AevkTarget* t, int item, float x, float y, float w, float h,
                                   float min_depth, float max_depth);
AevkPipeline* aevk_pipeline_create_state(AevkDevice* d, AevkTarget* t,
                                         const void* vert_spv, size_t vert_len,
                                         const void* frag_spv, size_t frag_len,
                                         const AevkLayout* layout, int push_bytes,
                                         const AevkBindings* bindings, const AevkState* state);

/* Stages the push-constant block used by the next draw. At most 128 bytes,
 * the minimum every Vulkan device guarantees. */
int aevk_target_set_push(AevkTarget* t, const void* data, size_t len);

/* --- geometry ------------------------------------------------------------ */

/* Uploads interleaved vertices (5 floats each: x, y, r, g, b) into a
 * host-visible buffer the GPU reads directly, owned by the target. Replaces
 * any previous upload and grows the allocation when it has to. */
int aevk_target_set_vertices(AevkTarget* t, const float* data, int vertex_count);

/* --- draw + readback ------------------------------------------------------ */

/* Records and submits one frame: clear to (r,g,b,a), draw the uploaded
 * vertices with `pipe`, then copy the image into the readback buffer. Blocks
 * on a fence until the GPU is done. The command buffer is recorded once per
 * distinct draw and reused, so a repeated call re-submits without re-recording.
 */
int aevk_draw(AevkTarget* t, AevkPipeline* pipe,
              float r, float g, float b, float a);

/* --- frames in flight ------------------------------------------------------ */

/* How many frames may be in flight at once, 1..8. One is the default and the
 * synchronous shape: `draw` records, submits and waits. Above one, `submit`
 * returns without waiting so the CPU can record the next frame while the GPU
 * works, and each slot carries its own readback buffer (width*height*4) so a
 * later frame cannot overwrite pixels an earlier one has not had read yet. */
int aevk_target_set_frames(AevkTarget* t, int count);
int aevk_target_frames(const AevkTarget* t);

/* The fence wait in milliseconds; the default 5000 is a hang detector rather
 * than a frame budget. */
int aevk_target_set_timeout_ms(AevkTarget* t, int ms);

/* Records and submits one frame without waiting. Returns the slot used, or a
 * negative status. Blocks only when every slot is still in flight. */
int aevk_submit(AevkTarget* t, AevkPipeline* p, float r, float g, float b, float a);

/* Waits for every outstanding frame. The readers (`pixel`, `read_rgba`,
 * `save_ppm`) wait for the most recent frame on their own, so this is for a
 * caller that wants the queue drained rather than the pixels. */
int aevk_wait_all(AevkTarget* t);

/* --- buffers and compute (#1515) --------------------------------------------- */

/* A buffer shaders read and write: host-visible, mapped for its lifetime and
 * zeroed at creation. Usable as a storage or uniform binding by a compute
 * pass and by a graphics pipeline, so compute output feeds a draw directly.
 * It must outlive every dispatch and draw that uses it; destroying one waits
 * for the device to be idle. */
AevkBuffer* aevk_buffer_create(AevkDevice* dev, size_t bytes);
void        aevk_buffer_destroy(AevkBuffer* b);
size_t      aevk_buffer_size(const AevkBuffer* b);
int         aevk_buffer_write(AevkBuffer* b, size_t offset, const void* data, size_t len);
int         aevk_buffer_read(AevkBuffer* b, size_t offset, void* out, size_t len);

/* Declares a storage buffer binding (`layout(std430, binding = N) buffer`)
 * for a compute or graphics pipeline. */
int aevk_bindings_storage(AevkBindings* b, int binding);

/* Points a storage or uniform binding of a graphics pipeline at a buffer. */
int aevk_material_set_buffer(AevkMaterial* m, int binding, AevkBuffer* buf);
int aevk_pipeline_set_buffer(AevkPipeline* p, int binding, AevkBuffer* buf);

/* A compute pipeline from SPIR-V (entry point `main`), the resources it
 * reads and writes, and a push-constant block of `push_bytes` (0..128, a
 * multiple of 4). It owns one descriptor set. */
AevkCompute* aevk_compute_create(AevkDevice* dev, const void* spv, size_t len,
                                 const AevkBindings* bindings, int push_bytes);
void         aevk_compute_destroy(AevkCompute* c);
int          aevk_compute_set_buffer(AevkCompute* c, int binding, AevkBuffer* buf);
int          aevk_compute_set_texture(AevkCompute* c, int binding, AevkTexture* tex);
int          aevk_compute_set_push(AevkCompute* c, const void* data, size_t len);
int          aevk_compute_set_timeout_ms(AevkCompute* c, int ms);

/* Runs gx * gy * gz work groups and waits for them; the buffers hold the
 * results when it returns. Every declared binding must have been set, and
 * the counts must be within the device's maxComputeWorkGroupCount. */
int aevk_dispatch(AevkCompute* c, int gx, int gy, int gz);

/* The same without waiting, so the CPU works while the GPU computes;
 * aevk_compute_wait before reading the results or dispatching again. */
int aevk_dispatch_async(AevkCompute* c, int gx, int gy, int gz);
int aevk_compute_wait(AevkCompute* c);

/* --- presentation (#1505) --------------------------------------------------- */

/* A surface over a window someone else owns, and a swapchain on it. `kind` is
 * an AEVK_WINDOW_* value and `display` / `window` are the handles that kind
 * takes; the window must outlive the swapchain. `width` x `height` is used
 * only where the window system leaves the size to the application (Wayland);
 * elsewhere the swapchain takes the window's own size.
 *
 * On macOS an NSView is given a CAMetalLayer, which must happen on the main
 * thread, as AppKit requires. Fails with AEVK_ERR_UNSUPPORTED, naming the
 * extension, when the loader or device cannot present to that kind. */
AevkSwapchain* aevk_swapchain_create(AevkDevice* dev, int kind, void* display,
                                     void* window, int width, int height);
void           aevk_swapchain_destroy(AevkSwapchain* sc);

/* The window changed size: rebuild now at the new size. */
int aevk_swapchain_resize(AevkSwapchain* sc, int width, int height);

/* On (the default): FIFO, one frame per vertical blank, never tearing. Off:
 * MAILBOX where offered, else IMMEDIATE, else FIFO. */
int aevk_swapchain_set_vsync(AevkSwapchain* sc, int on);

/* The current size in pixels; 0 x 0 while the window has no area (it is
 * minimised), when presenting is skipped rather than failed. */
int aevk_swapchain_width(const AevkSwapchain* sc);
int aevk_swapchain_height(const AevkSwapchain* sc);

/* The VkFormat of the swapchain images, and how many frames have reached the
 * presentation engine. For tests and diagnostics. */
int       aevk_swapchain_format(const AevkSwapchain* sc);
long long aevk_swapchain_presented(const AevkSwapchain* sc);

/* Shows the target's most recent frame in the window: acquires an image,
 * copies the frame into it (scaled when the sizes differ) and queues it for
 * presentation, without waiting for the GPU. Call it after draw() or
 * submit(). A swapchain that went out of date is rebuilt here, and the
 * encoding of its images follows the target's (sRGB for an sRGB target) so a
 * colour reaches the screen as it was written. */
int aevk_present(AevkSwapchain* sc, AevkTarget* t);

/* Copies the most recent frame from the mapped readback buffer in the
 * target's own format: aevk_rgba_size bytes. `out_len` must be at least that;
 * anything smaller is AEVK_ERR_ARG rather than a truncated read. */
int aevk_read_rgba(AevkTarget* t, void* out, size_t out_len);

/* The same frame as 8-bit RGBA whatever the format, width*height*4 bytes,
 * float channels clamped to 0..1: what an image file takes. */
int aevk_read_rgba8(AevkTarget* t, void* out, size_t out_len);

/* Bytes a full readback in the target's format needs:
 * width * height * bytes_per_pixel. */
size_t aevk_rgba_size(const AevkTarget* t);

#ifdef __cplusplus
}
#endif

#endif /* AETHER_VULKAN_H */
