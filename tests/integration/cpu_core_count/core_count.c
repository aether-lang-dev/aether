/* Exercise the real detector with synthetic OS and package counts. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../../runtime/config/aether_optimization_config.h"
#ifndef _WIN32
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#if AETHER_HAS_SIMD && (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86))
#define MOCK_X86 1
static int package_count;
static int online_count;
static void mock_cpuid(unsigned leaf, unsigned *a, unsigned *b, unsigned *c, unsigned *d) {
    *a = leaf == 0 ? 1 : 0;
    *b = leaf == 1 ? (unsigned)package_count << 16 : 0;
    *c = *d = 0;
}
#if defined(_MSC_VER)
#include <intrin.h>
static void mock_cpuidex(int regs[4], int leaf, int subleaf) {
    (void)subleaf;
    unsigned a, b, c, d;
    mock_cpuid((unsigned)leaf, &a, &b, &c, &d);
    regs[0] = (int)a; regs[1] = (int)b; regs[2] = (int)c; regs[3] = (int)d;
}
#define __cpuidex mock_cpuidex
#else
#include <cpuid.h>
#undef __cpuid_count
#define __cpuid_count(leaf, subleaf, a, b, c, d) \
    ((void)(subleaf), mock_cpuid(leaf, &(a), &(b), &(c), &(d)))
#endif

#if defined(_WIN32)
static void mock_system_info(SYSTEM_INFO *info) {
    memset(info, 0, sizeof(*info));
    info->dwNumberOfProcessors = online_count > 0 ? (DWORD)online_count : 0;
}
#define GetNativeSystemInfo mock_system_info
#elif defined(__APPLE__)
static int mock_sysctl(int *mib, unsigned n, void *out, size_t *len, void *in, size_t inlen) {
    (void)mib; (void)n; (void)len; (void)in; (void)inlen;
    if (online_count < 1) return -1;
    *(unsigned *)out = (unsigned)online_count;
    return 0;
}
#define sysctl mock_sysctl
// cpu_recommend_cores' macOS fallback (when the detector reports < 1 core)
// asks sysctlbyname("hw.perflevel0.physicalcpu"). Left real it would read
// the host's P-core count, so the "OS query failed -> 1" case would see
// live hardware instead of the mock. Fail it in lockstep with mock_sysctl.
static int mock_sysctlbyname(const char *name, void *out, size_t *len,
                             void *in, size_t inlen) {
    (void)name; (void)len; (void)in; (void)inlen;
    if (online_count < 1) return -1;
    *(int *)out = online_count;
    return 0;
}
#define sysctlbyname mock_sysctlbyname
#elif defined(_SC_NPROCESSORS_ONLN)
static long mock_sysconf(int name) {
    assert(name == _SC_NPROCESSORS_ONLN);
    return online_count;
}
#define sysconf mock_sysconf
#else
#undef MOCK_X86
#endif
#endif

#include "../../../runtime/utils/aether_cpu_detect.c"

int main(void) {
#ifdef MOCK_X86
    const int cases[][3] = {
        {1, 12, 12}, {8, 4, 4}, {64, 32, 16}, {8, -1, 8}, {0, -1, 1}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        package_count = cases[i][0];
        online_count = cases[i][1];
        g_cpu_info_initialized = 0;
        memset(&g_cpu_info, 0, sizeof(g_cpu_info));
        assert(cpu_recommend_cores() == cases[i][2]);
    }
#else
    assert(cpu_recommend_cores() >= 1 && cpu_recommend_cores() <= 16);
#endif
    puts("PASS: OS core count, package fallback, and scheduler cap");
    return 0;
}
