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
void          aevk_pipeline_destroy(AevkPipeline* p);

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
