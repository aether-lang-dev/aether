/* aether_process_mem.c — how much memory the process holds (#2310).
 * The contract of each call is in aether_process_mem.h. */
#include "aether_process_mem.h"

/* Under a sanitizer the sanitizer's allocator serves every malloc, and the
 * platform's statistics describe an allocator nothing uses (ASan's
 * `mallinfo` interceptor returns zeros). Read the sanitizer's own count. */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define AETHER_SANITIZER_ALLOCATOR 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
      __has_feature(memory_sanitizer)
#    define AETHER_SANITIZER_ALLOCATOR 1
#  endif
#endif
#ifdef AETHER_SANITIZER_ALLOCATOR
#  include <sanitizer/allocator_interface.h>
#endif

#if defined(_WIN32)
/* PSAPI_VERSION 2 maps GetProcessMemoryInfo to kernel32's
 * K32GetProcessMemoryInfo, so nothing links against psapi.dll. */
#  ifndef PSAPI_VERSION
#    define PSAPI_VERSION 2
#  endif
#  include <windows.h>
#  include <psapi.h>
#  include <stdlib.h>
#elif defined(__APPLE__)
#  include <malloc/malloc.h>
#  include <mach/mach.h>
#elif defined(__FreeBSD__)
#  include <sys/types.h>
#  include <sys/sysctl.h>
#  include <sys/user.h>
#  include <malloc_np.h>
#  include <unistd.h>
#elif defined(__EMSCRIPTEN__)
#  include <malloc.h>
#elif defined(__linux__)
#  include <stdio.h>
#  include <unistd.h>
#  if defined(__GLIBC__)
#    include <malloc.h>
#  endif
#endif

int64_t aether_heap_in_use(void) {
#if defined(AETHER_SANITIZER_ALLOCATOR)
    return (int64_t)__sanitizer_get_current_allocated_bytes();
#elif defined(_WIN32)
    /* The CRT allocates from the process heap (UCRT) or a heap of its own
     * (older msvcrt); a C library an extern reaches may create more. Sum
     * the allocated bytes of every heap the process has. */
    HANDLE local[64];
    HANDLE* heaps = local;
    DWORD n = GetProcessHeaps(64, local);
    if (n == 0) return -1;
    if (n > 64) {
        heaps = (HANDLE*)malloc(sizeof(HANDLE) * n);
        if (!heaps) return -1;
        DWORD got = GetProcessHeaps(n, heaps);
        /* Heaps created in between: read the ones that fit. */
        if (got < n) n = got;
    }
    int64_t total = 0;
    for (DWORD i = 0; i < n; i++) {
        HEAP_SUMMARY hs;
        ZeroMemory(&hs, sizeof(hs));
        hs.cb = sizeof(hs);
        if (HeapSummary(heaps[i], 0, &hs)) total += (int64_t)hs.cbAllocated;
    }
    if (heaps != local) free(heaps);
    return total;
#elif defined(__APPLE__)
    malloc_statistics_t st;
    malloc_zone_statistics(NULL, &st);   /* NULL: every zone */
    return (int64_t)st.size_in_use;
#elif defined(__FreeBSD__)
    /* jemalloc refreshes its statistics when the epoch advances. */
    uint64_t epoch = 1;
    size_t esz = sizeof(epoch);
    if (mallctl("epoch", &epoch, &esz, &epoch, esz) != 0) return -1;
    size_t allocated = 0;
    size_t sz = sizeof(allocated);
    if (mallctl("stats.allocated", &allocated, &sz, NULL, 0) != 0) return -1;
    return (int64_t)allocated;
#elif defined(__EMSCRIPTEN__)
    struct mallinfo mi = mallinfo();
    return (int64_t)(unsigned)mi.uordblks;
#elif defined(__linux__) && defined(__GLIBC__)
    /* In use = the arena's allocated chunks plus the chunks too large for
     * the arena, which glibc maps one by one. */
#  if __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33)
    struct mallinfo2 mi = mallinfo2();
    return (int64_t)mi.uordblks + (int64_t)mi.hblkhd;
#  else
    struct mallinfo mi = mallinfo();   /* int fields: exact below 2 GiB */
    return (int64_t)(unsigned)mi.uordblks + (int64_t)(unsigned)mi.hblkhd;
#  endif
#else
    return -1;   /* musl and others expose no allocator statistics */
#endif
}

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
/* Field `which` (0-based) of /proc/self/statm, in pages; -1 on failure. */
static int64_t statm_pages(int which) {
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long long v[3] = { -1, -1, -1 };
    int got = fscanf(f, "%lld %lld %lld", &v[0], &v[1], &v[2]);
    fclose(f);
    if (got != 3 || which < 0 || which > 2) return -1;
    return (int64_t)v[which];
}
#endif

int64_t aether_process_resident(void) {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return -1;
    return (int64_t)pmc.WorkingSetSize;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) != KERN_SUCCESS) return -1;
    return (int64_t)info.resident_size;
#elif defined(__FreeBSD__)
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)getpid() };
    struct kinfo_proc kp;
    size_t len = sizeof(kp);
    if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0) return -1;
    return (int64_t)kp.ki_rssize * (int64_t)getpagesize();
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
    int64_t pages = statm_pages(1);
    return pages < 0 ? -1 : pages * (int64_t)sysconf(_SC_PAGESIZE);
#else
    return -1;
#endif
}

int64_t aether_process_private(void) {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc,
                              sizeof(pmc))) return -1;
    return (int64_t)pmc.PrivateUsage;
#elif defined(__APPLE__)
    /* The physical footprint: what Activity Monitor calls Memory. */
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  (task_info_t)&info, &count) != KERN_SUCCESS) return -1;
    return (int64_t)info.phys_footprint;
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
    int64_t resident = statm_pages(1);
    int64_t shared = statm_pages(2);
    if (resident < 0 || shared < 0) return -1;
    return (resident - shared) * (int64_t)sysconf(_SC_PAGESIZE);
#else
    return -1;
#endif
}
