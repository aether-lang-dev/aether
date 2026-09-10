// CPU Feature Detection for Runtime Optimization
// Detects AVX2, AVX-512, and other CPU capabilities

#include "../config/aether_optimization_config.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#if defined(__APPLE__) || defined(__MACOS__)
#include <sys/sysctl.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

#if AETHER_HAS_SIMD
   /* CPUID intrinsics come from different headers per toolchain. MSVC exposes
    * __cpuidex via <intrin.h>; a GNU compiler (gcc/clang, INCLUDING zig cc's
    * MinGW target and MSYS2 mingw) exposes __cpuid_count via <cpuid.h> and does
    * NOT declare __cpuidex from <intrin.h>. Keying on _WIN32 alone picked the
    * MSVC path for mingw/zig and broke the cross-Windows build with
    * "call to undeclared function '__cpuidex'". Split on the compiler, not the
    * OS: MSVC -> <intrin.h>; GNU on x86 -> <cpuid.h>. */
#  if defined(_MSC_VER)
#    include <intrin.h>
#  elif defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#    include <cpuid.h>
#  endif
#endif

typedef struct {
    int avx_supported;
    int avx2_supported;
    int avx512f_supported;
    int fma_supported;
    int sse42_supported;
    int mwait_supported;
    int monitor_supported;
    char cpu_brand[49];
    int num_cores;
    int cache_line_size;
} CPUInfo;

static CPUInfo g_cpu_info = {0};
static int g_cpu_info_initialized = 0;

#if AETHER_HAS_SIMD
// CPUID wrapper
static void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    /* MSVC path (its <intrin.h> __cpuidex). A GNU compiler on Windows
     * (mingw/zig cc) falls through to the __cpuid_count path below, exactly
     * like native Linux — matching the header split above. */
#if defined(_MSC_VER)
    int regs[4];
    __cpuidex(regs, leaf, subleaf);
    *eax = regs[0];
    *ebx = regs[1];
    *ecx = regs[2];
    *edx = regs[3];
#elif defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    __cpuid_count(leaf, subleaf, *eax, *ebx, *ecx, *edx);
#else
    // ARM or other architecture - return zeros
    *eax = 0;
    *ebx = 0;
    *ecx = 0;
    *edx = 0;
#endif
}
#endif // AETHER_HAS_SIMD

// Detect CPU features
void cpu_detect_features(CPUInfo* info) {
#if !AETHER_HAS_SIMD
    // Minimal detection when SIMD is unavailable (WASM, embedded)
    strncpy(info->cpu_brand, "Unknown (SIMD disabled)", sizeof(info->cpu_brand) - 1);
    info->cpu_brand[sizeof(info->cpu_brand) - 1] = '\0';
    info->avx_supported = 0;
    info->avx2_supported = 0;
    info->avx512f_supported = 0;
    info->fma_supported = 0;
    info->sse42_supported = 0;
    info->mwait_supported = 0;
    info->monitor_supported = 0;
    info->cache_line_size = 64;
    // Try to get core count from OS
#if defined(__EMSCRIPTEN__)
    info->num_cores = 1;  // WASM is single-threaded
#elif defined(__linux__)
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    info->num_cores = (nprocs > 0) ? (int)nprocs : 1;
#elif defined(__APPLE__)
    int mib[2] = {CTL_HW, HW_NCPU};
    unsigned int ncpu = 0;
    size_t len = sizeof(ncpu);
    if (sysctl(mib, 2, &ncpu, &len, NULL, 0) == 0 && ncpu > 0)
        info->num_cores = ncpu;
    else
        info->num_cores = 1;
#elif defined(_WIN32)
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    info->num_cores = (int)si.dwNumberOfProcessors;
#else
    info->num_cores = 1;
#endif
    return;
#elif defined(__aarch64__) || defined(__arm64__) || defined(__arm__)
    // ARM architecture - use OS APIs for detection
    strncpy(info->cpu_brand, "ARM Processor", sizeof(info->cpu_brand) - 1);
    info->cpu_brand[sizeof(info->cpu_brand) - 1] = '\0';

    // Get core count via OS
#if defined(__APPLE__)
    int mib[2] = {CTL_HW, HW_NCPU};
    unsigned int ncpu = 0;
    size_t len = sizeof(ncpu);
    if (sysctl(mib, 2, &ncpu, &len, NULL, 0) == 0 && ncpu > 0) {
        info->num_cores = ncpu;
    } else {
        info->num_cores = 1;
    }
    // Apple Silicon has 128-byte cache lines (performance cores)
    info->cache_line_size = 128;
#elif defined(__linux__)
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    info->num_cores = (nprocs > 0) ? (int)nprocs : 1;
    info->cache_line_size = 64;  // Most ARM Linux uses 64-byte cache lines
#else
    info->num_cores = 1;
    info->cache_line_size = 64;
#endif

    // ARM SIMD (NEON) detection
#if defined(__aarch64__) || defined(__arm64__)
    // ARMv8/AArch64 always has NEON (Advanced SIMD)
    info->sse42_supported = 1;  // NEON is comparable to SSE4.2
    info->avx_supported = 0;    // No AVX on ARM
    info->avx2_supported = 0;   // No AVX2 on ARM
    info->avx512f_supported = 0;
    info->fma_supported = 1;    // ARMv8 has FMA
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    // ARM32 with NEON (compile-time detection)
    info->sse42_supported = 1;  // NEON provides SIMD
    info->avx_supported = 0;
    info->avx2_supported = 0;
    info->avx512f_supported = 0;
    info->fma_supported = 0;    // VFPv4 has FMA but not all ARMv7
#else
    // ARM32 without NEON compile flag - try runtime detection on Linux
    info->sse42_supported = 0;
    info->avx_supported = 0;
    info->avx2_supported = 0;
    info->avx512f_supported = 0;
    info->fma_supported = 0;
#if defined(__linux__)
    // Check /proc/cpuinfo for NEON support
    FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo) {
        char line[256];
        while (fgets(line, sizeof(line), cpuinfo)) {
            if (strncmp(line, "Features", 8) == 0 && strstr(line, "neon")) {
                info->sse42_supported = 1;  // NEON available
                break;
            }
        }
        fclose(cpuinfo);
    }
#endif
#endif

    info->mwait_supported = 0;  // No MWAIT on ARM, use WFE instead
    info->monitor_supported = 0;

#else
    // x86 architecture - use CPUID
    uint32_t eax, ebx, ecx, edx;

    // Get CPU vendor and max CPUID level
    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_level = eax;

    // Get CPU brand string
    cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000004) {
        uint32_t* brand = (uint32_t*)info->cpu_brand;
        cpuid(0x80000002, 0, &brand[0], &brand[1], &brand[2], &brand[3]);
        cpuid(0x80000003, 0, &brand[4], &brand[5], &brand[6], &brand[7]);
        cpuid(0x80000004, 0, &brand[8], &brand[9], &brand[10], &brand[11]);
        info->cpu_brand[48] = '\0';
    } else {
        strncpy(info->cpu_brand, "Unknown CPU", sizeof(info->cpu_brand) - 1);
        info->cpu_brand[sizeof(info->cpu_brand) - 1] = '\0';
    }

    // Check CPUID feature flags
    if (max_level >= 1) {
        cpuid(1, 0, &eax, &ebx, &ecx, &edx);

        // ECX flags
        info->avx_supported = (ecx & (1 << 28)) != 0;  // AVX
        info->fma_supported = (ecx & (1 << 12)) != 0;  // FMA3
        info->sse42_supported = (ecx & (1 << 20)) != 0;  // SSE4.2
        info->monitor_supported = (ecx & (1 << 3)) != 0;  // MONITOR/MWAIT
        info->mwait_supported = info->monitor_supported;  // Same flag

        // Logical processor count
        info->num_cores = (ebx >> 16) & 0xFF;

        // Cache line size (bits 8-15 of EBX, in 8-byte units)
        info->cache_line_size = ((ebx >> 8) & 0xFF) * 8;
        if (info->cache_line_size == 0) {
            info->cache_line_size = 64;  // Default to 64 bytes
        }
    }

    // Extended features (leaf 7)
    if (max_level >= 7) {
        cpuid(7, 0, &eax, &ebx, &ecx, &edx);

        // EBX flags
        info->avx2_supported = (ebx & (1 << 5)) != 0;      // AVX2
        info->avx512f_supported = (ebx & (1 << 16)) != 0;  // AVX-512 Foundation
    }
#endif
}

// Initialize and cache CPU info
const CPUInfo* cpu_get_info(void) {
    if (!g_cpu_info_initialized) {
        cpu_detect_features(&g_cpu_info);
        g_cpu_info_initialized = 1;
    }
    return &g_cpu_info;
}

// Check if AVX2 is available
int cpu_has_avx2(void) {
    return cpu_get_info()->avx2_supported;
}

// Check if AVX-512 is available
int cpu_has_avx512(void) {
    return cpu_get_info()->avx512f_supported;
}

// Check if MONITOR/MWAIT is available
int cpu_has_mwait(void) {
    return cpu_get_info()->mwait_supported;
}

// Check if SSE4.2 is available
int cpu_has_sse42(void) {
    return cpu_get_info()->sse42_supported;
}

// Print CPU capabilities
void cpu_print_info(void) {
    const CPUInfo* info = cpu_get_info();
    
    printf("=== CPU Information ===\n");
    printf("CPU: %s\n", info->cpu_brand);
    printf("Logical cores: %d\n\n", info->num_cores);
    
    printf("SIMD Support:\n");
    printf("  SSE4.2:  %s\n", info->sse42_supported ? "YES" : "NO");
    printf("  AVX:     %s\n", info->avx_supported ? "YES" : "NO");
    printf("  AVX2:    %s", info->avx2_supported ? "YES" : "NO");
    if (info->avx2_supported) {
        printf(" -> 256-bit SIMD batch processing available");
    }
    printf("\n");

    printf("  AVX-512: %s", info->avx512f_supported ? "YES" : "NO");
    if (info->avx512f_supported) {
        printf(" -> 512-bit SIMD batch processing available");
    }
    printf("\n");
    
    printf("  FMA3:    %s\n", info->fma_supported ? "YES" : "NO");
    
    printf("\nPower Management:\n");
    printf("  MWAIT:   %s", info->mwait_supported ? "YES" : "NO");
    if (info->mwait_supported) {
        printf(" -> Sub-microsecond idle wake latency");
    }
    printf("\n");
    
    printf("\nCache:\n");
    printf("  Line size: %d bytes\n", info->cache_line_size);
    
    printf("\nRecommendation:\n");
    if (info->avx2_supported) {
        printf("  Enable SIMD with: aether_runtime_init(cores, AETHER_FLAG_ENABLE_SIMD);\n");
        printf("  Run benchmarks/cross-language to measure throughput on this host.\n");
    } else {
        printf("  SIMD not available, using scalar code.\n");
        printf("  Run benchmarks/cross-language to measure throughput on this host.\n");
    }
}

// Recommend optimal core count
int cpu_recommend_cores(void) {
    const CPUInfo* info = cpu_get_info();

    // Use all logical cores, but cap at 16 for diminishing returns
    int cores = info->num_cores;

    // CPUID returns 0 on non-x86 (ARM, virtualized) — fall back to OS API
    if (cores < 1) {
#if defined(__linux__)
        long sc = sysconf(_SC_NPROCESSORS_ONLN);
        cores = (sc > 0) ? (int)sc : 1;
#elif defined(__APPLE__) || defined(__MACOS__)
        // On Apple Silicon, prefer P-cores only for consistent performance
        // Query hw.perflevel0.physicalcpu (Performance cores)
        int pcore_count = 0;
        size_t len = sizeof(pcore_count);
        if (sysctlbyname("hw.perflevel0.physicalcpu", &pcore_count, &len, NULL, 0) == 0 && pcore_count > 0) {
            // Apple Silicon - use P-cores only to avoid E-core slowdowns
            cores = pcore_count;
        } else {
            // Not Apple Silicon or query failed - use total cores
            int mib[2] = {CTL_HW, HW_NCPU};
            unsigned int val = 0;
            len = sizeof(val);
            if (sysctl(mib, 2, &val, &len, NULL, 0) == 0 && val > 0)
                cores = (int)val;
            else
                cores = 1;
        }
#elif defined(_WIN32)
        SYSTEM_INFO si;
        GetNativeSystemInfo(&si);
        cores = (int)si.dwNumberOfProcessors;
#else
        cores = 1;
#endif
    }

    if (cores > 16) cores = 16;
    if (cores < 1) cores = 1;

    return cores;
}
