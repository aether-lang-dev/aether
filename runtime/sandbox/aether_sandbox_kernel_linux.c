// aether_sandbox_kernel_linux.c: the kernel layer of a sandboxed spawn
// (Linux). See aether_sandbox_kernel.h for the split between parent and
// child, and docs/containment-sandbox.md for what is held and why.

#if defined(__linux__) && !defined(__ANDROID__)

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

#include "aether_sandbox_kernel.h"

extern int list_size(void*);
extern void* list_get_raw(void*, int);
extern const char* aether_string_data(const void*);

int aether_sandbox_has_category(void* grant_list, const char* category) {
    if (!grant_list) return 0;
    int n = list_size(grant_list);
    if (n <= 0 || n % 2 != 0) return 0;
    for (int i = 0; i < n; i += 2) {
        const char* cat = aether_string_data(list_get_raw(grant_list, i));
        const char* pat = aether_string_data(list_get_raw(grant_list, i + 1));
        if (!cat || !pat) continue;
        if (strcmp(cat, "*") == 0 && strcmp(pat, "*") == 0) return 1;
        if (strcmp(cat, category) == 0) return 1;
    }
    return 0;
}

// ---- seccomp: the process-creation fence ----
//
// The filter refuses the child a new process: fork, vfork and a clone that
// is not a thread get EPERM.
//
// A thread is no new process (#3178): it shares the address space, the
// filter and the preload, and cannot leave them. So a clone with
// CLONE_THREAD in its flags (its first argument, on every ABI here) passes.
// clone3 keeps its flags in a struct in memory, which a filter cannot read,
// so it gets ENOSYS, the answer of a kernel without clone3, and libc falls
// back to clone, whose flags the filter can read.
//
// A seccomp filter sees the syscall's ABI (seccomp_data.arch) and its number
// in that ABI, and a process can enter the kernel through more than one ABI.
// So the filter dispatches on arch:
//   - the native ABI traps its own numbers;
//   - on x86_64, i386 (int 0x80, a static 32-bit binary) traps the i386
//     numbers, and x32 calls (native arch, number | 0x40000000) are caught
//     by masking that bit off before comparing;
//   - on aarch64, 32-bit ARM (compat) traps the ARM numbers;
//   - any other arch kills the process: an ABI we did not list is one we
//     cannot fence.
//
// Syscall numbers are ABI literals, not SYS_* macros: those resolve to the
// build host's ABI, and the generic table (arm64, riscv64, loongarch64) has
// no fork/vfork at all. tests/integration/sandbox_clone_fence checks every
// literal below against the kernel headers zig ships for that architecture.
// Each list is clone first, clone3 last, and the plain refusals between.
//   x86_64:  clone 56, fork 57, vfork 58, clone3 435
//   i386 and 32-bit ARM: clone 120, fork 2, vfork 190, clone3 435
//   generic (aarch64, riscv64, loongarch64): clone 220, clone3 435
#ifndef AUDIT_ARCH_AARCH64
#define AUDIT_ARCH_AARCH64     0xC00000B7u
#endif
#ifndef AUDIT_ARCH_ARM
#define AUDIT_ARCH_ARM         0x40000028u
#endif
#ifndef AUDIT_ARCH_RISCV64
#define AUDIT_ARCH_RISCV64     0xC00000F3u
#endif
#ifndef AUDIT_ARCH_LOONGARCH64
#define AUDIT_ARCH_LOONGARCH64 0xC0000102u
#endif
#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS SECCOMP_RET_KILL
#endif

#define FENCE_MAX 48
#define X32_SYSCALL_BIT 0x40000000u

#define FENCE_CLONE_THREAD 0x00010000u   /* CLONE_THREAD, the same on every ABI */

/* The low 32 bits of a syscall's first argument, where CLONE_THREAD is. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define FENCE_ARG0_LOW ((uint32_t)offsetof(struct seccomp_data, args) + 4u)
#else
#define FENCE_ARG0_LOW ((uint32_t)offsetof(struct seccomp_data, args))
#endif

static struct sock_filter fence_jeq(uint32_t k, int pc, int target) {
    /* A jump is relative to the instruction after it. */
    return (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, k,
                                        (uint8_t)(target - (pc + 1)), 0);
}

// One arch's block, from `nums` (clone first, clone3 last, plain refusals
// between): load the number (masked when `mask`); a clone goes on to the
// CLONE_THREAD test, clone3 gets ENOSYS, a plain refusal EPERM, anything
// else is allowed.
static int fence_block(struct sock_filter* f, int n, const uint32_t* nums, int count,
                       uint32_t mask) {
    int deny_count = count - 2;
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                          (uint32_t)offsetof(struct seccomp_data, nr));
    if (mask) f[n++] = (struct sock_filter)BPF_STMT(BPF_ALU | BPF_AND | BPF_K, mask);
    int p = n;                               /* the first comparison */
    int allow = p + 2 + deny_count;          /* RET ALLOW */
    int check = allow + 1;                   /* LD the clone's flags */
    int jset = check + 1;                    /* JSET CLONE_THREAD */
    int allow_thread = jset + 1;             /* RET ALLOW */
    int deny = allow_thread + 1;             /* RET EPERM */
    int enosys = deny + 1;                   /* RET ENOSYS */
    f[n] = fence_jeq(nums[0], n, check);
    n++;
    f[n] = fence_jeq(nums[count - 1], n, enosys);
    n++;
    for (int i = 1; i <= deny_count; i++) {
        f[n] = fence_jeq(nums[i], n, deny);
        n++;
    }
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, FENCE_ARG0_LOW);
    f[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, FENCE_CLONE_THREAD,
                                        (uint8_t)(allow_thread - (n + 1)),
                                        (uint8_t)(deny - (n + 1)));
    n++;
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                          SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                          SECCOMP_RET_ERRNO | (ENOSYS & SECCOMP_RET_DATA));
    return n;
}

// Build the fence into `out`. Its length, or -1 on an architecture this
// filter does not know (the caller refuses to run the child unfenced).
static int build_clone_fence(struct sock_filter* out, int cap) {
    static const uint32_t x86_64_nums[]  = { 56, 57, 58, 435 };
    static const uint32_t legacy32_nums[] = { 120, 2, 190, 435 };   /* i386, ARM */
    static const uint32_t generic_nums[] = { 220, 435 };
    (void)x86_64_nums; (void)legacy32_nums; (void)generic_nums; (void)cap;

#if defined(__x86_64__) && !defined(__ILP32__)
    uint32_t native = AUDIT_ARCH_X86_64, compat = AUDIT_ARCH_I386;
    const uint32_t* nnums = x86_64_nums; int ncount = 4; uint32_t nmask = ~X32_SYSCALL_BIT;
    const uint32_t* cnums = legacy32_nums; int ccount = 4;
#elif defined(__aarch64__)
    uint32_t native = AUDIT_ARCH_AARCH64, compat = AUDIT_ARCH_ARM;
    const uint32_t* nnums = generic_nums; int ncount = 2; uint32_t nmask = 0;
    const uint32_t* cnums = legacy32_nums; int ccount = 4;
#elif defined(__riscv) && __riscv_xlen == 64
    uint32_t native = AUDIT_ARCH_RISCV64, compat = 0;
    const uint32_t* nnums = generic_nums; int ncount = 2; uint32_t nmask = 0;
    const uint32_t* cnums = NULL; int ccount = 0;
#elif defined(__loongarch64) || (defined(__loongarch__) && defined(__loongarch_lp64))
    uint32_t native = AUDIT_ARCH_LOONGARCH64, compat = 0;
    const uint32_t* nnums = generic_nums; int ncount = 2; uint32_t nmask = 0;
    const uint32_t* cnums = NULL; int ccount = 0;
#else
    /* No fence for this architecture: refuse, rather than run the child with
     * only the libc-level fence while the caller asked for containment. */
    (void)out;
    return -1;
#endif

#if defined(__x86_64__) || defined(__aarch64__) || defined(__riscv) || defined(__loongarch__)
    /* Assemble: [0] LD arch, [1] JEQ native, [2] JEQ compat (if any),
     * then KILL, then the native block, then the compat block. */
    struct sock_filter nb[FENCE_MAX], cb[FENCE_MAX];
    int nn = fence_block(nb, 0, nnums, ncount, nmask);
    int cn = cnums ? fence_block(cb, 0, cnums, ccount, 0) : 0;

    struct sock_filter* f = out;
    int n = 0;
    int head = compat ? 4 : 3;            /* LD, JEQ, [JEQ], KILL */
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                          (uint32_t)offsetof(struct seccomp_data, arch));
    /* jt is relative to the next instruction. */
    f[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, native,
                                        (uint8_t)(head - (n + 1)), 0);
    n++;
    if (compat) {
        f[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, compat,
                                            (uint8_t)(head + nn - (n + 1)), 0);
        n++;
    }
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    memcpy(f + n, nb, (size_t)nn * sizeof(nb[0]));
    n += nn;
    if (cn) {
        memcpy(f + n, cb, (size_t)cn * sizeof(cb[0]));
        n += cn;
    }
    return n;
#endif
}

// ---- prepare / apply / release ----

int aether_sandbox_kernel_prepare(AetherSandboxKernel* k, void* grant_list,
                                  int flags, char* err, size_t errsz) {
    (void)flags;
    memset(k, 0, sizeof *k);
    if (err && errsz) err[0] = '\0';

    // grant_all() is the equivalent of no sandbox, and a fork grant needs
    // no fence (the fence is all the filter holds today).
    if (aether_sandbox_has_category(grant_list, "fork")) return 0;

    int n = build_clone_fence(k->filter, AETHER_SBX_FILTER_MAX);
    if (n < 0) {
        if (err) snprintf(err, errsz,
                          "no seccomp filter for this architecture, so the child "
                          "cannot be fenced; grant 'fork:*' to opt out");
        return -1;
    }
    k->seccomp = 1;
    k->filter_len = (unsigned short)n;
    return 0;
}

int aether_sandbox_kernel_apply(const AetherSandboxKernel* k) {
    if (!k->seccomp) return 0;
    struct sock_fprog prog = { .len = k->filter_len,
                               .filter = (struct sock_filter*)k->filter };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return AETHER_SBX_STEP_SECCOMP;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) return AETHER_SBX_STEP_SECCOMP;
    return 0;
}

void aether_sandbox_kernel_release(AetherSandboxKernel* k) {
    (void)k;
}

const char* aether_sandbox_kernel_failure(int step, int e, char* buf, size_t bufsz) {
    const char* what = step == AETHER_SBX_STEP_SECCOMP
        ? "cannot install the seccomp filter (needs Linux 3.5+ with "
          "PR_SET_NO_NEW_PRIVS and PR_SET_SECCOMP)"
        : "cannot apply the kernel layer";
    snprintf(buf, bufsz, "%s: %s", what, strerror(e));
    return buf;
}

#endif // __linux__ && !__ANDROID__
