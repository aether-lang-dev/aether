/* libaether_caps.c — public-API forwarders for issue #343 resource
 * caps. The implementation lives in aether_resource_caps.c; this
 * file's only job is to expose the internal symbols under the
 * stable libaether.h names that embedders link against.
 *
 * The internal API uses unprefixed function names so the rest of
 * the runtime + stdlib (which links the same module) can call them
 * cheaply. The public surface uses `aether_set_*` / `aether_deadline_*`
 * — that's the documented, ABI-stable shape.
 */

/* Resolved through the include path, NOT as `../include/libaether.h`: that
 * spelling only works in the source tree. In an install the runtime sits at
 * share/aether/runtime/ while headers are under include/aether/, so the
 * relative hop pointed at a directory that does not exist and every
 * cross-compile died here (#1420). Native builds never noticed, because they
 * pass the include set from `ae cflags` and never rely on the relative path. */
#include "libaether.h"
#include "aether_resource_caps.h"

AETHER_API void aether_set_memory_cap(uint64_t bytes) {
    aether_caps_set_memory_cap(bytes);
}

AETHER_API void aether_set_call_deadline(int64_t ms) {
    aether_caps_set_deadline_ms(ms);
}

AETHER_API int aether_deadline_tripped(void) {
    return aether_caps_deadline_tripped();
}
