/* LLVMFuzzerTestOneInput for an Aether fuzz target (run_fuzz_c.sh): the
 * target, built --emit=obj, exports fuzz_target(data: ptr, len: int), whose
 * C symbol is aether_fuzz_target. The input reaches it whole, NUL bytes
 * included. */
#include <stdint.h>
#include <stddef.h>

extern void aether_fuzz_target(const void* data, int32_t len);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 0x7fffffff) return 0;
    aether_fuzz_target((const void*)data, (int32_t)size);
    return 0;
}
