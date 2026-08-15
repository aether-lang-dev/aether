// Example: Using Aether Runtime Configuration
// Shows how to control optimizations with flags

#include <stdio.h>
#include "../aether_runtime.h"
#include "../utils/aether_cpu_detect.h"

void print_examples() {
    printf("========================================\n");
    printf("  Aether Runtime Configuration Examples\n");
    printf("========================================\n\n");
    
    printf("1. AUTO-DETECT (Recommended):\n");
    printf("   aether_runtime_init(0, AETHER_FLAG_AUTO_DETECT);\n");
    printf("   - Detects your CPU and enables best optimizations\n");
    printf("   - Lock-free mailboxes, SIMD, MWAIT all auto-enabled\n");
    printf("   - Zero configuration needed\n\n");
    
    printf("2. MAXIMUM PERFORMANCE:\n");
    printf("   aether_runtime_init(8, \n");
    printf("       AETHER_FLAG_LOCKFREE_MAILBOX |\n");
    printf("       AETHER_FLAG_LOCKFREE_POOLS |\n");
    printf("       AETHER_FLAG_ENABLE_SIMD |\n");
    printf("       AETHER_FLAG_ENABLE_MWAIT);\n");
    printf("   - Explicit control, all optimizations ON\n");
    printf("   - Best for modern CPUs (2013+)\n\n");
    
    printf("3. COMPATIBILITY MODE:\n");
    printf("   aether_runtime_init(4, 0);\n");
    printf("   - All optimizations OFF\n");
    printf("   - Works on any CPU, even old systems\n");
    printf("   - Slower but guaranteed compatibility\n\n");
    
    printf("4. SELECTIVE OPTIMIZATION:\n");
    printf("   aether_runtime_init(8, \n");
    printf("       AETHER_FLAG_LOCKFREE_MAILBOX |\n");
    printf("       AETHER_FLAG_LOCKFREE_POOLS);\n");
    printf("   - Lock-free only, no SIMD/MWAIT\n");
    printf("   - Good for non-x86 platforms\n\n");
    
    printf("5. DEBUG/VERBOSE:\n");
    printf("   aether_runtime_init(0, \n");
    printf("       AETHER_FLAG_AUTO_DETECT |\n");
    printf("       AETHER_FLAG_VERBOSE);\n");
    printf("   - Prints configuration on startup\n");
    printf("   - Helpful for debugging performance issues\n\n");
    
    printf("========================================\n\n");
}

int main() {
    print_examples();
    
    // Example 1: Auto-detect (best for most users)
    printf("Running Example 1: AUTO-DETECT\n\n");
    aether_runtime_init(0, AETHER_FLAG_AUTO_DETECT | AETHER_FLAG_VERBOSE);
    
    printf("\n");
    
    // Show what flags are available
    printf("Available Flags:\n");
    printf("  AETHER_FLAG_AUTO_DETECT      (0x%02x) - Auto-detect CPU features\n", AETHER_FLAG_AUTO_DETECT);
    printf("  AETHER_FLAG_LOCKFREE_MAILBOX (0x%02x) - Lock-free SPSC mailboxes\n", AETHER_FLAG_LOCKFREE_MAILBOX);
    printf("  AETHER_FLAG_LOCKFREE_POOLS   (0x%02x) - Lock-free TLS pools\n", AETHER_FLAG_LOCKFREE_POOLS);
    printf("  AETHER_FLAG_ENABLE_SIMD      (0x%02x) - AVX2 vectorization\n", AETHER_FLAG_ENABLE_SIMD);
    printf("  AETHER_FLAG_ENABLE_MWAIT     (0x%02x) - MWAIT idle strategy\n", AETHER_FLAG_ENABLE_MWAIT);
    printf("  AETHER_FLAG_VERBOSE          (0x%02x) - Print config on init\n", AETHER_FLAG_VERBOSE);
    
    printf("\nHow to use in your code:\n\n");
    printf("  int main() {\n");
    printf("      // Initialize runtime\n");
    printf("      aether_runtime_init(0, AETHER_FLAG_AUTO_DETECT);\n");
    printf("      \n");
    printf("      // Actors come from the Aether compiler: an `actor` block\n");
    printf("      // becomes a struct with a mailbox, and uses whichever\n");
    printf("      // optimizations the flags above turned on.\n");
    printf("      \n");
    printf("      // ... your code ...\n");
    printf("      \n");
    printf("      // Cleanup\n");
    printf("      aether_runtime_shutdown();\n");
    printf("      return 0;\n");
    printf("  }\n\n");
    
    printf("That's it! No recompilation needed for different CPUs.\n");
    
    aether_runtime_shutdown();
    return 0;
}
