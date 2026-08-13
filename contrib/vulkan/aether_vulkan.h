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
 * colour, stride 20. `format` is a VkFormat value. */
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
void         aevk_texture_destroy(AevkTexture* tex);
int          aevk_texture_upload(AevkTexture* tex, const void* rgba, size_t len);

/* Writes a uniform buffer, creating and binding it on first use, then
 * copying on every call. Host-coherent, so a per-frame update is a memcpy
 * and no descriptor rewrite. */
int aevk_pipeline_set_uniform(AevkPipeline* p, int binding, const void* data, size_t len);

/* Points a combined-image-sampler binding at `tex`. */
int aevk_pipeline_set_texture(AevkPipeline* p, int binding, AevkTexture* tex);

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

/* Copies width*height*4 bytes of R8G8B8A8 from the mapped readback buffer.
 * `out_len` must be at least that; anything smaller is AEVK_ERR_ARG rather
 * than a truncated read. */
int aevk_read_rgba(AevkTarget* t, void* out, size_t out_len);

/* Bytes a full readback needs: width * height * 4. */
size_t aevk_rgba_size(const AevkTarget* t);

#ifdef __cplusplus
}
#endif

#endif /* AETHER_VULKAN_H */
