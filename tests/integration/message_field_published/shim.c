/* Reads a string's header for the probe: whether its count is the atomic,
 * published one (#2919, #3192). */
#include "aether_string.h"

/* 1 for a published counted string, 0 for a plain-counted one, -1 for
 * anything else (a literal, a C string). The count is read as the runtime
 * reads it, atomically: a published string's count is changed by the
 * receiver's thread while this one looks (a plain read raced it, #3542). */
int probe_published(const void* s) {
    if (!s) return -1;
    const AetherString* h = (const AetherString*)s;
    if (h->magic != AETHER_STRING_MAGIC) return -1;
    int rc = aether_string_count_now(&h->ref_count);
    if (rc == AETHER_STRING_PINNED_REFS) return -1;
    return (rc & AETHER_STRING_SHARED_BIT) ? 1 : 0;
}
