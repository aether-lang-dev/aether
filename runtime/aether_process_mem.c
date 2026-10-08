/* aether_process_mem.c — how much memory the process holds (#2310).
 * The contract of each call is in aether_process_mem.h. */

/* glibc's dlfcn.h declares RTLD_DEFAULT only under _GNU_SOURCE, and a
 * feature macro counts only before the first system header. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

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
/* The sanitizer runtimes' public allocator statistic, declared here rather
 * than through <sanitizer/allocator_interface.h>: GCC's packages do not
 * always ship that header, while every ASan/TSan/MSan runtime exports the
 * function. */
#  include <stddef.h>
size_t __sanitizer_get_current_allocated_bytes(void);
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
#  include <fcntl.h>
#  include <string.h>
#  if defined(__GLIBC__)
#    include <malloc.h>
#    include <dlfcn.h>
#    include <stdlib.h>
#    include <string.h>
#  endif
#endif

#if defined(__linux__) && defined(__GLIBC__) && !defined(AETHER_SANITIZER_ALLOCATOR)
/* glibc's statistics describe glibc's arenas, which is only the answer when
 * glibc serves malloc. An allocator preloaded in front of it (jemalloc,
 * tcmalloc, mimalloc) owns `malloc`, and valgrind redirects glibc's malloc
 * to its own from a preload object named vgpreload_<tool>-<platform>.so;
 * in both, glibc's numbers stand still while the program allocates. */
static int glibc_serves_malloc(void) {
    void* m = dlsym(RTLD_DEFAULT, "malloc");
    void* libc_m = dlsym(RTLD_DEFAULT, "__libc_malloc");
    if (m && libc_m && m != libc_m) return 0;
    const char* preload = getenv("LD_PRELOAD");
    if (preload && strstr(preload, "vgpreload")) return 0;
    return 1;
}
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
    /* The busy blocks of each heap, walked. HeapSummary gives the same
     * number without the walk, but Wine exports it as a stub that aborts
     * the process when called, so it cannot even be tried. The walk costs
     * time in proportion to the heap's block count: a diagnostic read, not
     * one for a hot path. */
    int64_t total = 0;
    int read = 0;
    for (DWORD i = 0; i < n; i++) {
        if (!HeapLock(heaps[i])) continue;
        PROCESS_HEAP_ENTRY e;
        e.lpData = NULL;
        while (HeapWalk(heaps[i], &e)) {
            if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) total += (int64_t)e.cbData;
            read = 1;
        }
        HeapUnlock(heaps[i]);
    }
    if (heaps != local) free(heaps);
    return read ? total : -1;
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
    if (!glibc_serves_malloc()) return -1;
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

/* Whether aether_heap_in_use() counts exactly the blocks the program holds.
 * A sanitizer's allocator counts live allocations, and Windows' heap walk
 * counts busy blocks. glibc, macOS and jemalloc report from statistics that
 * also count freed blocks parked in per-thread caches; on a churning
 * workload those settle over many rounds, so growth between two rounds is
 * not, by itself, a leak there. */
int aether_heap_in_use_exact(void) {
#if defined(AETHER_SANITIZER_ALLOCATOR)
    return aether_heap_in_use() >= 0;
#elif defined(_WIN32)
    /* Windows' heap walk reports every block. Wine's reports a
     * low-fragmentation group, which it turns on per size class once that
     * class is busy, as one block of its whole size whatever it holds
     * (dlls/ntdll/heap.c, heap_walk_blocks), so under Wine the count moves
     * by groups and two steady rounds can differ without a leak. Wine's
     * ntdll exports wine_get_version; Windows' does not. */
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll && GetProcAddress(ntdll, "wine_get_version")) return 0;
    return aether_heap_in_use() >= 0;
#else
    return 0;
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

#if defined(_WIN32)
/* Every thread that starts or ends in the process: the program's, and the
 * ones the system starts in it (a thread-pool worker, a loader worker). The
 * loader calls a TLS callback for each, as it calls DllMain, so the count
 * sees threads no Aether code created. A TLS callback is an entry in the
 * image's .CRT$XL* table, which the C runtime's TLS directory (_tls_used)
 * hands to the loader; winpthreads registers its own the same way. */
static volatile LONG g_thread_events = 0;

static void NTAPI aether_thread_event(PVOID module, DWORD reason, PVOID reserved) {
    (void)module;
    (void)reserved;
    if (reason == DLL_THREAD_ATTACH || reason == DLL_THREAD_DETACH)
        InterlockedIncrement(&g_thread_events);
}

#  if defined(_MSC_VER)
#    if defined(_M_IX86)
#      pragma comment(linker, "/INCLUDE:__tls_used")
#      pragma comment(linker, "/INCLUDE:_aether_thread_event_callback")
#    else
#      pragma comment(linker, "/INCLUDE:_tls_used")
#      pragma comment(linker, "/INCLUDE:aether_thread_event_callback")
#    endif
#    pragma section(".CRT$XLB", long, read)
__declspec(allocate(".CRT$XLB")) const PIMAGE_TLS_CALLBACK aether_thread_event_callback = aether_thread_event;
#  else
/* The reference links the C runtime's TLS directory, whose callback list
 * runs from .CRT$XLA to .CRT$XLZ; without one, nothing pulls it in and the
 * image has no callbacks at all. */
extern const IMAGE_TLS_DIRECTORY _tls_used;
__attribute__((used)) static const void* const aether_tls_directory = &_tls_used;
__attribute__((section(".CRT$XLB"), used))
PIMAGE_TLS_CALLBACK aether_thread_event_callback = aether_thread_event;
#  endif
#endif

int64_t aether_thread_epoch(void) {
#if defined(_WIN32)
    return (int64_t)InterlockedCompareExchange(&g_thread_events, 0, 0);
#elif defined(__APPLE__)
    thread_act_array_t list;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return -1;
    for (mach_msg_type_number_t i = 0; i < count; i++) mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(thread_act_t));
    return (int64_t)count;
#elif defined(__FreeBSD__)
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)getpid() };
    struct kinfo_proc kp;
    size_t len = sizeof(kp);
    if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0) return -1;
    return (int64_t)kp.ki_numthreads;
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
    /* Field 20 of /proc/self/stat, num_threads. Read with open and read,
     * not stdio, so the read allocates nothing the heap count would see.
     * The command name (field 2) is parenthesised and may hold spaces and
     * parentheses itself, so the fields are counted from its last ')'. */
    char buf[1024];
    int fd = open("/proc/self/stat", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    char* p = strrchr(buf, ')');
    if (!p) return -1;
    int field = 2;
    while (*p && field < 20) {
        if (*p == ' ') field++;
        p++;
    }
    if (field != 20) return -1;
    long long v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return (int64_t)v;
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
