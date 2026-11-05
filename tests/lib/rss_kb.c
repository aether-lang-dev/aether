/* getrusage_max_rss_kb: the process's CURRENT resident set in KB, after
 * asking the allocator to hand its cached free pages back to the OS.
 * Shared by the no-leak probes, which compare it before and after a long
 * loop; each names this file in its aether.toml's extra_sources.
 *
 * The name is historical. It is not getrusage's ru_maxrss: that is a
 * peak, which never falls, and it counts pages the allocator has freed
 * but kept resident. A leak-free malloc/free loop still raises it. On
 * FreeBSD, jemalloc's dirty pages lifted it by about 3.2 MB across any
 * loop of 100k or more short strings, the same at 100k as at 400k, and
 * that failed heap_leak_interp_as_arg's 1 MB bound with no leak at all.
 * Current RSS taken after a purge falls back when memory is freed, so a
 * leak-free loop returns near its baseline and a real leak shows as
 * growth in proportion to what it keeps.
 *
 *   macOS     malloc_zone_pressure_relief, then task_info resident_size
 *   FreeBSD   jemalloc tcache flush and purge of every arena, then
 *             kinfo_proc's ki_rssize (no /proc needed)
 *   Linux     glibc's malloc_trim, then /proc/self/statm
 *   Windows   -1: the probes then run smoke-only
 *   other     ru_maxrss, the peak, as the old shims did */

#if defined(_WIN32) || defined(__MINGW32__) || defined(__MINGW64__)

int getrusage_max_rss_kb(void) { return -1; }

#elif defined(__APPLE__) && defined(__MACH__)

#include <mach/mach.h>
#include <malloc/malloc.h>

int getrusage_max_rss_kb(void) {
    malloc_zone_pressure_relief(malloc_default_zone(), 0);
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) != KERN_SUCCESS) {
        return -1;
    }
    return (int)(info.resident_size / 1024);
}

#elif defined(__FreeBSD__)

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <malloc_np.h>
#include <stdio.h>
#include <unistd.h>

int getrusage_max_rss_kb(void) {
    char purge[64];
    snprintf(purge, sizeof purge, "arena.%u.purge", (unsigned)MALLCTL_ARENAS_ALL);
    (void)mallctl("thread.tcache.flush", NULL, NULL, NULL, 0);
    (void)mallctl(purge, NULL, NULL, NULL, 0);
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)getpid() };
    struct kinfo_proc kp;
    size_t len = sizeof kp;
    if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0 || len != sizeof kp) return -1;
    return (int)((long)kp.ki_rssize * (sysconf(_SC_PAGESIZE) / 1024));
}

#elif defined(__linux__)

#include <stdio.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

int getrusage_max_rss_kb(void) {
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long total_pages = 0, resident_pages = 0;
    int got = fscanf(f, "%ld %ld", &total_pages, &resident_pages);
    fclose(f);
    if (got != 2) return -1;
    return (int)(resident_pages * (sysconf(_SC_PAGESIZE) / 1024));
}

#else

#include <sys/resource.h>

int getrusage_max_rss_kb(void) {
    struct rusage r;
    if (getrusage(RUSAGE_SELF, &r) != 0) return -1;
    return (int)r.ru_maxrss;
}

#endif
