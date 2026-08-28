#ifndef AETHER_CPU_AVAILABLE_H
#define AETHER_CPU_AVAILABLE_H

/* How many CPUs this process may actually run on.
 *
 * This is not the same question as how many CPUs the machine has, and sizing
 * a thread pool with the latter is wrong wherever the two differ:
 *
 *   - a cpuset (`taskset`, `docker --cpuset-cpus`, a Kubernetes pod pinned to
 *     a core set) leaves the machine's CPU count untouched, so a host with 64
 *     CPUs and a 2-CPU set gets 64 threads fighting over 2;
 *   - a CFS quota (`docker --cpus=2`, a Kubernetes CPU limit) is the common
 *     case and is not a cpuset at all: affinity still reports every CPU while
 *     the process is throttled to a fraction of one.
 *
 * Oversubscribing this way does not merely waste memory. Threads that would
 * have run become threads that are preempted, and the involuntary context
 * switches and the scheduler time land in the request path.
 *
 * Header-only on purpose: the callers live in std/ and in the runtime, and
 * those are not in one link set (see http_pool_worker_count). A header keeps
 * one definition rather than a copy per caller.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(__linux__)
/* The affinity mask is read through the raw system call rather than
 * sched_getaffinity(3) and CPU_COUNT. Those need _GNU_SOURCE to be defined
 * before <sched.h> is first pulled in by anything in the translation unit,
 * and a header cannot guarantee it got there first: when it does not, the
 * macros are absent, the calls compile as implicit declarations and the
 * build fails at link time. The system call has no such requirement. */
#include <sys/syscall.h>

/* Declared here rather than taken from <unistd.h>, which only exposes it when
 * a feature-test macro was set before the first include in the translation
 * unit. Every libc this builds against declares it exactly this way, and a
 * second identical declaration is harmless. */
extern long syscall(long, ...);
#endif

#if defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/cpuset.h>
#endif

#if defined(__linux__)
/* CPUs in this process's affinity mask; 0 when it cannot be read. The call
 * reports how many bytes of mask it wrote, and only those are counted. */
static inline int aether_affinity_count(void) {
    unsigned long mask[128];   /* 8192 CPUs, well past any real machine */
    memset(mask, 0, sizeof(mask));
    long written = syscall(SYS_sched_getaffinity, 0, (unsigned)sizeof(mask), mask);
    if (written <= 0) return 0;

    size_t words = (size_t)written / sizeof(unsigned long);
    if (words > sizeof(mask) / sizeof(mask[0])) words = sizeof(mask) / sizeof(mask[0]);

    int n = 0;
    for (size_t i = 0; i < words; i++) {
        unsigned long w = mask[i];
        while (w) { n += (int)(w & 1UL); w >>= 1; }
    }
    return n;
}

/* A cgroup CPU limit, in whole CPUs, rounded up; 0 when there is none.
 * Rounded up because a limit of 1.5 CPUs can still keep two threads busy,
 * while rounding down to 1 would leave half the allowance unused. */
static inline int aether_cgroup_cpu_limit(void) {
    double quota = 0.0, period = 0.0;

    /* cgroup v2: "$MAX $PERIOD", where $MAX is "max" when unlimited. */
    FILE* f = fopen("/sys/fs/cgroup/cpu.max", "re");
    if (f) {
        char word[64];
        double p = 0.0;
        if (fscanf(f, "%63s %lf", word, &p) == 2 && p > 0.0 && strcmp(word, "max") != 0) {
            quota = atof(word);
            period = p;
        }
        fclose(f);
    }

    /* cgroup v1: quota and period in separate files, quota -1 when unlimited. */
    if (period <= 0.0) {
        double q = 0.0, p = 0.0;
        f = fopen("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", "re");
        if (f) { if (fscanf(f, "%lf", &q) != 1) q = 0.0; fclose(f); }
        f = fopen("/sys/fs/cgroup/cpu/cpu.cfs_period_us", "re");
        if (f) { if (fscanf(f, "%lf", &p) != 1) p = 0.0; fclose(f); }
        if (q > 0.0 && p > 0.0) { quota = q; period = p; }
    }

    if (quota <= 0.0 || period <= 0.0) return 0;

    double cpus = quota / period;
    if (cpus < 1.0) return 1;
    int n = (int)cpus;
    if (cpus > (double)n) n++;
    return n;
}
#endif

static inline int aether_cpu_available(void) {
    int n = 0;

#if defined(_WIN32)
    /* A process affinity mask covers one processor group; without the mask
     * the group's CPU count is the honest answer. */
    DWORD_PTR proc_mask = 0, sys_mask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask) && proc_mask) {
        while (proc_mask) { n += (int)(proc_mask & 1u); proc_mask >>= 1; }
    } else {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        n = (int)si.dwNumberOfProcessors;
    }
#else
#if defined(__linux__)
    n = aether_affinity_count();
#elif defined(__FreeBSD__) && defined(CPU_COUNT)
    cpuset_t set;
    CPU_ZERO(&set);
    if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1,
                           sizeof(set), &set) == 0) n = CPU_COUNT(&set);
#endif
    if (n <= 0) {
#if defined(_SC_NPROCESSORS_ONLN)
        long sc = sysconf(_SC_NPROCESSORS_ONLN);
        if (sc > 0) n = (int)sc;
#endif
    }
#endif

    if (n <= 0) n = 1;

#if defined(__linux__)
    int quota = aether_cgroup_cpu_limit();
    if (quota > 0 && quota < n) n = quota;
#endif

    return n;
}

#endif // AETHER_CPU_AVAILABLE_H
