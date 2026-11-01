#include <stdint.h>
#include <stddef.h>

// Forward declaration of the Aether export.
// Under --emit=lib / --emit=obj, the exported C symbol is aether_<name>.
extern void aether_fuzz_target(const void* data, int32_t len);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 0x7fffffff) return 0;
    aether_fuzz_target((const void*)data, (int32_t)size);
    return 0;
}
