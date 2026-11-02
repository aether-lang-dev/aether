// aether_sandbox_kernel_linux.c: the kernel layer of a sandboxed spawn
// (Linux). See aether_sandbox_kernel.h for the split between parent and
// child, and docs/containment-sandbox.md for what is held and why.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* O_PATH */
#endif
#if defined(__linux__) && !defined(__ANDROID__)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

#include "aether_sandbox_kernel.h"
#include "../aether_sandbox_match.h"     // aether_split_host_port
#include "../utils/aether_spawn.h"       // aether_spawn_resolve

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

// ---- seccomp ----
//
// The filter holds the syscalls the preload cannot see, however they are
// made (a raw syscall instruction, glibc's own inline __vfork, a static
// binary). What it refuses, unless a grant says otherwise:
//
//   clone       a new process (a clone without CLONE_THREAD) unless
//               grant_fork; a new namespace (CLONE_NEW*) always
//   clone3      ENOSYS always: its flags are in memory, which a filter cannot
//               read, and libc falls back to clone on ENOSYS
//   fork, vfork EPERM unless grant_fork
//   execve      EPERM unless grant_exec, except the one execve that starts
//               the program (below)
//   execveat    EPERM unless grant_exec
//   socket      AF_INET and AF_INET6 unless a tcp, udp or listen grant;
//               AF_UNIX and the rest pass (the channel is a Unix socket)
//   socketcall  (i386) EPERM unless a network grant: its arguments are in
//               memory
//   unshare     EPERM for a new namespace (CLONE_NEW*); other flags pass
//   ptrace, process_vm_readv, process_vm_writev, bpf, mount, umount2,
//   pivot_root, setns, keyctl, add_key, request_key, open_tree,
//   move_mount, fsopen, fsconfig, fsmount, mount_setattr, userfaultfd,
//   perf_event_open: EPERM always
//   io_uring_setup, io_uring_enter, io_uring_register: ENOSYS always, the
//               answer of a kernel without io_uring, so libuv and others
//               fall back to plain calls
//
// The program's own execve. The filter is installed before the child execs
// its program, and that execve must pass. So without grant_exec, execve is
// allowed only when its first argument is the very pointer the child passes
// (the resolved program, or /bin/sh for a file with no #! line), compared in
// all 64 bits. After the exec that pointer means nothing, and the preload's
// constructor installs a second filter that refuses execve and execveat
// outright (AETHER_SANDBOX_NO_EXEC=1, libaether_sandbox_preload.c), before
// any of the program's code runs. A static program, which the preload never
// enters, keeps only the pointer check, and Landlock, which lets it execute
// nothing but its own file and loader.
//
// A seccomp filter sees the syscall's ABI (seccomp_data.arch) and its number
// in that ABI, and a process can enter the kernel through more than one ABI.
// So the filter dispatches on arch:
//   - the native ABI traps its own numbers;
//   - on x86_64, i386 (int 0x80, a static 32-bit binary) traps the i386
//     numbers, and x32 calls (native arch, number | 0x40000000) are caught
//     by masking that bit off before comparing, with x32's own numbers for
//     the calls where they differ;
//   - on aarch64, 32-bit ARM (compat) traps the ARM numbers;
//   - any other arch kills the process: an ABI we did not list is one we
//     cannot fence.
// A compat ABI's execve is always refused there: the program's execve is
// native.
//
// Syscall numbers are ABI literals, not SYS_* macros: those resolve to the
// build host's ABI. tests/integration/sandbox_clone_fence checks every
// literal below against the kernel headers zig ships for that architecture.
// -1 is a call the ABI does not have.
//
// SBX_NAMES: clone clone3 fork vfork execve execveat socket socketcall ptrace process_vm_readv process_vm_writev bpf mount umount2 pivot_root unshare setns keyctl add_key request_key open_tree move_mount fsopen fsconfig fsmount mount_setattr userfaultfd perf_event_open io_uring_setup io_uring_enter io_uring_register
enum {
    SC_CLONE, SC_CLONE3, SC_FORK, SC_VFORK, SC_EXECVE, SC_EXECVEAT, SC_SOCKET, SC_SOCKETCALL,
    SC_PTRACE, SC_PVM_READV, SC_PVM_WRITEV, SC_BPF, SC_MOUNT, SC_UMOUNT2, SC_PIVOT_ROOT,
    SC_UNSHARE, SC_SETNS, SC_KEYCTL, SC_ADD_KEY, SC_REQUEST_KEY, SC_OPEN_TREE, SC_MOVE_MOUNT,
    SC_FSOPEN, SC_FSCONFIG, SC_FSMOUNT, SC_MOUNT_SETATTR, SC_USERFAULTFD, SC_PERF_EVENT_OPEN,
    SC_IO_URING_SETUP, SC_IO_URING_ENTER, SC_IO_URING_REGISTER,
    SC_COUNT
};
static const int x86_64_nr[SC_COUNT] = { 56, 435, 57, 58, 59, 322, 41, -1, 101, 310, 311, 321, 165, 166, 155, 272, 308, 250, 248, 249, 428, 429, 430, 431, 432, 442, 323, 298, 425, 426, 427 };
static const int i386_nr[SC_COUNT] = { 120, 435, 2, 190, 11, 358, 359, 102, 26, 347, 348, 357, 21, 52, 217, 310, 346, 288, 286, 287, 428, 429, 430, 431, 432, 442, 374, 336, 425, 426, 427 };
static const int arm_nr[SC_COUNT] = { 120, 435, 2, 190, 11, 387, 281, -1, 26, 376, 377, 386, 21, 52, 218, 337, 375, 311, 309, 310, 428, 429, 430, 431, 432, 442, 388, 364, 425, 426, 427 };
static const int generic_nr[SC_COUNT] = { 220, 435, -1, -1, 221, 281, 198, -1, 117, 270, 271, 280, 40, 39, 41, 97, 268, 219, 217, 218, 428, 429, 430, 431, 432, 442, 282, 241, 425, 426, 427 };
// x32's own numbers, where they differ from x86_64's (after the x32 bit is
// masked off): execve, execveat, ptrace, process_vm_readv, process_vm_writev.
// SBX_X32_NAMES: execve execveat ptrace process_vm_readv process_vm_writev
static const int x32_nr[5] = { 520, 545, 521, 539, 540 };
static const int x32_sc[5] = { SC_EXECVE, SC_EXECVEAT, SC_PTRACE, SC_PVM_READV, SC_PVM_WRITEV };

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

#define X32_SYSCALL_BIT 0x40000000u
#define SBX_CLONE_THREAD 0x00010000u
/* CLONE_NEWNS, NEWCGROUP, NEWUTS, NEWIPC, NEWUSER, NEWPID, NEWNET; unshare
 * also takes CLONE_NEWTIME (0x80), which in clone's flags is CSIGNAL. */
#define SBX_CLONE_NEW   0x7E020000u
#define SBX_UNSHARE_NEW (SBX_CLONE_NEW | 0x00000080u)
#define SBX_AF_INET  2u
#define SBX_AF_INET6 10u

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define SBX_ARG0_LO ((uint32_t)offsetof(struct seccomp_data, args) + 4u)
#define SBX_ARG0_HI ((uint32_t)offsetof(struct seccomp_data, args))
#else
#define SBX_ARG0_LO ((uint32_t)offsetof(struct seccomp_data, args))
#define SBX_ARG0_HI ((uint32_t)offsetof(struct seccomp_data, args) + 4u)
#endif

typedef struct {
    int fork, exec, net;
    uint64_t ptrs[2];
    int nptrs;
} SbxPolicy;

#define SBX_STMT(c, k) ((struct sock_filter)BPF_STMT((c), (k)))
#define SBX_RET(v) SBX_STMT(BPF_RET | BPF_K, (v))
#define SBX_EPERM SBX_RET(SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA))
#define SBX_ENOSYS SBX_RET(SECCOMP_RET_ERRNO | (ENOSYS & SECCOMP_RET_DATA))
#define SBX_ALLOW SBX_RET(SECCOMP_RET_ALLOW)

static struct sock_filter sbx_jump(uint32_t op, uint32_t k, int pc, int jt, int jf) {
    /* Targets are absolute in the block; a jump counts from the next one. */
    return (struct sock_filter)BPF_JUMP(BPF_JMP | op | BPF_K, k,
                                        (uint8_t)(jt - (pc + 1)), (uint8_t)(jf - (pc + 1)));
}

// The instructions for one syscall's action into `b`, each ending in a
// return; their count, 0 for one that is allowed.
static int sbx_action(struct sock_filter* b, int sc, const SbxPolicy* p, int native) {
    int n = 0;
    switch (sc) {
    case SC_CLONE:
        b[n++] = SBX_STMT(BPF_LD | BPF_W | BPF_ABS, SBX_ARG0_LO);
        if (p->fork) {
            b[n] = sbx_jump(BPF_JSET, SBX_CLONE_NEW, n, 3, 2); n++;
            b[n++] = SBX_ALLOW;
            b[n++] = SBX_EPERM;
        } else {
            b[n] = sbx_jump(BPF_JSET, SBX_CLONE_NEW, n, 4, 2); n++;
            b[n] = sbx_jump(BPF_JSET, SBX_CLONE_THREAD, n, 3, 4); n++;
            b[n++] = SBX_ALLOW;
            b[n++] = SBX_EPERM;
        }
        return n;
    case SC_UNSHARE:
        b[n++] = SBX_STMT(BPF_LD | BPF_W | BPF_ABS, SBX_ARG0_LO);
        b[n] = sbx_jump(BPF_JSET, SBX_UNSHARE_NEW, n, 3, 2); n++;
        b[n++] = SBX_ALLOW;
        b[n++] = SBX_EPERM;
        return n;
    case SC_CLONE3:
    case SC_IO_URING_SETUP: case SC_IO_URING_ENTER: case SC_IO_URING_REGISTER:
        b[n++] = SBX_ENOSYS;
        return n;
    case SC_FORK: case SC_VFORK:
        if (p->fork) return 0;
        b[n++] = SBX_EPERM;
        return n;
    case SC_EXECVEAT:
        if (p->exec) return 0;
        b[n++] = SBX_EPERM;
        return n;
    case SC_EXECVE: {
        if (p->exec) return 0;
        if (!native || p->nptrs == 0) { b[n++] = SBX_EPERM; return n; }
        // LD lo; JEQ lo_i -> hi_i (else the next, the last to EPERM); per
        // pointer: LD hi; JEQ hi_i -> ALLOW : EPERM; ALLOW; EPERM. (A BPF
        // jump only goes forward.)
        int k = p->nptrs;
        int hi0 = 1 + k;                  /* first hi check (2 instructions each) */
        int allow = hi0 + 2 * k;          /* RET ALLOW */
        int deny = allow + 1;             /* RET EPERM */
        b[n++] = SBX_STMT(BPF_LD | BPF_W | BPF_ABS, SBX_ARG0_LO);
        for (int i = 0; i < k; i++) {
            b[n] = sbx_jump(BPF_JEQ, (uint32_t)p->ptrs[i], n, hi0 + 2 * i, i + 1 < k ? n + 1 : deny);
            n++;
        }
        for (int i = 0; i < k; i++) {
            b[n++] = SBX_STMT(BPF_LD | BPF_W | BPF_ABS, SBX_ARG0_HI);
            b[n] = sbx_jump(BPF_JEQ, (uint32_t)(p->ptrs[i] >> 32), n, allow, deny);
            n++;
        }
        b[n++] = SBX_ALLOW;
        b[n++] = SBX_EPERM;
        return n;
    }
    case SC_SOCKET:
        if (p->net) return 0;
        b[n++] = SBX_STMT(BPF_LD | BPF_W | BPF_ABS, SBX_ARG0_LO);
        b[n] = sbx_jump(BPF_JEQ, SBX_AF_INET, n, 4, n + 1); n++;
        b[n] = sbx_jump(BPF_JEQ, SBX_AF_INET6, n, 4, 3); n++;
        b[n++] = SBX_ALLOW;
        b[n++] = SBX_EPERM;
        return n;
    case SC_SOCKETCALL:
        if (p->net) return 0;
        b[n++] = SBX_EPERM;
        return n;
    default:
        b[n++] = SBX_EPERM;
        return n;
    }
}

// One syscall: JEQ nr (else skip) and its action. -1 when out of room.
static int sbx_rule(struct sock_filter* f, int n, int cap, int nr, int sc,
                    const SbxPolicy* p, int native) {
    struct sock_filter act[16];
    int an = sbx_action(act, sc, p, native);
    if (an == 0 || nr < 0) return n;
    if (n + 1 + an > cap) return -1;
    f[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)nr, 0, (uint8_t)an);
    n++;
    memcpy(f + n, act, (size_t)an * sizeof act[0]);
    return n + an;
}

// One ABI's block: load the number (masked when `mask`), the rules, and
// allow whatever none of them names. -1 when out of room.
__attribute__((unused)) static int sbx_block(struct sock_filter* f, int n, int cap, const int* nrs, uint32_t mask,
                     int x32, const SbxPolicy* p, int native) {
    if (n + 3 > cap) return -1;
    f[n++] = SBX_STMT(BPF_LD | BPF_W | BPF_ABS, (uint32_t)offsetof(struct seccomp_data, nr));
    if (mask) f[n++] = SBX_STMT(BPF_ALU | BPF_AND | BPF_K, mask);
    for (int sc = 0; sc < SC_COUNT && n >= 0; sc++) n = sbx_rule(f, n, cap, nrs[sc], sc, p, native);
    for (int i = 0; x32 && i < 5 && n >= 0; i++) n = sbx_rule(f, n, cap, x32_nr[i], x32_sc[i], p, 0);
    if (n < 0 || n + 1 > cap) return -1;
    f[n++] = SBX_ALLOW;
    return n;
}

// The whole filter into `out`: its length, or -1 on an architecture it does
// not know (the caller refuses to run the child unfenced) or out of room.
static int build_filter(struct sock_filter* out, int cap, const SbxPolicy* p) {
    (void)x86_64_nr; (void)i386_nr; (void)arm_nr; (void)generic_nr; (void)x32_nr; (void)x32_sc;
#if defined(__x86_64__) && !defined(__ILP32__)
    uint32_t native = AUDIT_ARCH_X86_64, compat = AUDIT_ARCH_I386;
    const int* nnrs = x86_64_nr; uint32_t nmask = ~X32_SYSCALL_BIT; int x32 = 1;
    const int* cnrs = i386_nr;
#elif defined(__aarch64__)
    uint32_t native = AUDIT_ARCH_AARCH64, compat = AUDIT_ARCH_ARM;
    const int* nnrs = generic_nr; uint32_t nmask = 0; int x32 = 0;
    const int* cnrs = arm_nr;
#elif defined(__riscv) && __riscv_xlen == 64
    uint32_t native = AUDIT_ARCH_RISCV64, compat = 0;
    const int* nnrs = generic_nr; uint32_t nmask = 0; int x32 = 0;
    const int* cnrs = NULL;
#elif defined(__loongarch64) || (defined(__loongarch__) && defined(__loongarch_lp64))
    uint32_t native = AUDIT_ARCH_LOONGARCH64, compat = 0;
    const int* nnrs = generic_nr; uint32_t nmask = 0; int x32 = 0;
    const int* cnrs = NULL;
#else
    (void)out; (void)cap; (void)p;
    return -1;
#endif

#if defined(__x86_64__) || defined(__aarch64__) || defined(__riscv) || defined(__loongarch__)
    /* [0] LD arch, [1] JEQ native, [2] JEQ compat (if any), KILL, then the
     * native block, then the compat block. A jump is at most 255 ahead, so
     * the compat block's start is reached through the head only. */
    struct sock_filter nb[AETHER_SBX_FILTER_MAX];
    int nn = sbx_block(nb, 0, AETHER_SBX_FILTER_MAX, nnrs, nmask, x32, p, 1);
    if (nn < 0) return -1;
    int head = compat ? 4 : 3;
    if (head + nn > cap) return -1;
    int n = 0;
    out[n++] = SBX_STMT(BPF_LD | BPF_W | BPF_ABS, (uint32_t)offsetof(struct seccomp_data, arch));
    out[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, native,
                                          (uint8_t)(head - (n + 1)), 0);
    n++;
    int compat_jump = -1;
    if (compat) {
        compat_jump = n;
        out[n] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, compat, 0, 0);
        n++;
    }
    out[n++] = SBX_RET(SECCOMP_RET_KILL_PROCESS);
    memcpy(out + n, nb, (size_t)nn * sizeof nb[0]);
    n += nn;
    if (compat) {
        int at = n;
        if (at - (compat_jump + 1) > 255) {
            /* Too far for a jump: put the compat block first instead. */
            return -1;
        }
        out[compat_jump].jt = (uint8_t)(at - (compat_jump + 1));
        int cn = sbx_block(out, n, cap, cnrs, 0, 0, p, 0);
        if (cn < 0) return -1;
        n = cn;
    }
    return n;
#endif
}

// ---- Landlock ----
//
// Landlock (Linux 5.13+) restricts what files this process and its
// children may open, by the kernel and for an unprivileged process (no root,
// no user namespace). The grants become its rules, so a raw openat that the
// preload never sees is refused too. Network rules (bind/connect on TCP
// ports) are ABI 4 (Linux 6.7); scoping of abstract Unix sockets and signals
// is ABI 6 (Linux 6.12).
//
// The syscall numbers are the same on every architecture (444-446), and the
// ABI structs are spelt here, so the build needs no <linux/landlock.h>.
//
// How grants map (docs/containment-sandbox.md, "Landlock"):
//   fs_read   READ_FILE, READ_DIR
//   fs_write  WRITE_FILE, TRUNCATE, REMOVE_*, MAKE_*, REFER
//   fs        both
//   exec      EXECUTE, READ_FILE on the program the pattern names
//   tcp       CONNECT_TCP on the pattern's port (ABI 4); a pattern with no
//             port leaves connect to the preload and seccomp
//   tcp_listen BIND_TCP is then not restricted
// and how patterns map: "*" is "/"; "/dir/*" is the directory and what is
// under it; "/dir/name*" is each entry of /dir whose name starts with
// "name" when the child starts (later ones are not covered: narrower);
// an exact path is that file (an exact directory gets no rule, since a
// Landlock rule on a directory covers what is under it: narrower); a
// pattern that starts with "*" (a suffix) cannot be expressed and is an
// error under AETHER_SBX_STRICT.

#define LL_CREATE_RULESET 444
#define LL_ADD_RULE       445
#define LL_RESTRICT_SELF  446
#define LL_CREATE_RULESET_VERSION 1u
#define LL_RULE_PATH_BENEATH 1
#define LL_RULE_NET_PORT     2

#define LL_FS_EXECUTE     (1ULL << 0)
#define LL_FS_WRITE_FILE  (1ULL << 1)
#define LL_FS_READ_FILE   (1ULL << 2)
#define LL_FS_READ_DIR    (1ULL << 3)
#define LL_FS_REMOVE_DIR  (1ULL << 4)
#define LL_FS_REMOVE_FILE (1ULL << 5)
#define LL_FS_MAKE_CHAR   (1ULL << 6)
#define LL_FS_MAKE_DIR    (1ULL << 7)
#define LL_FS_MAKE_REG    (1ULL << 8)
#define LL_FS_MAKE_SOCK   (1ULL << 9)
#define LL_FS_MAKE_FIFO   (1ULL << 10)
#define LL_FS_MAKE_BLOCK  (1ULL << 11)
#define LL_FS_MAKE_SYM    (1ULL << 12)
#define LL_FS_REFER       (1ULL << 13)   /* ABI 2 */
#define LL_FS_TRUNCATE    (1ULL << 14)   /* ABI 3 */
#define LL_NET_BIND_TCP    (1ULL << 0)   /* ABI 4 */
#define LL_NET_CONNECT_TCP (1ULL << 1)
#define LL_SCOPE_ABSTRACT_UNIX_SOCKET (1ULL << 0)   /* ABI 6 */
#define LL_SCOPE_SIGNAL               (1ULL << 1)

#define LL_FS_V1 (LL_FS_EXECUTE | LL_FS_WRITE_FILE | LL_FS_READ_FILE | LL_FS_READ_DIR | \
                  LL_FS_REMOVE_DIR | LL_FS_REMOVE_FILE | LL_FS_MAKE_CHAR | LL_FS_MAKE_DIR | \
                  LL_FS_MAKE_REG | LL_FS_MAKE_SOCK | LL_FS_MAKE_FIFO | LL_FS_MAKE_BLOCK | \
                  LL_FS_MAKE_SYM)
/* The rights a rule on a file (not a directory) may carry. */
#define LL_FS_FILE (LL_FS_EXECUTE | LL_FS_WRITE_FILE | LL_FS_READ_FILE | LL_FS_TRUNCATE)

#define LL_READ  (LL_FS_READ_FILE | LL_FS_READ_DIR)
#define LL_WRITE (LL_FS_WRITE_FILE | LL_FS_TRUNCATE | LL_FS_REMOVE_DIR | LL_FS_REMOVE_FILE | \
                  LL_FS_MAKE_CHAR | LL_FS_MAKE_DIR | LL_FS_MAKE_REG | LL_FS_MAKE_SOCK | \
                  LL_FS_MAKE_FIFO | LL_FS_MAKE_BLOCK | LL_FS_MAKE_SYM | LL_FS_REFER)
#define LL_EXEC  (LL_FS_EXECUTE | LL_FS_READ_FILE)

struct ll_ruleset_attr {
    uint64_t handled_access_fs;
    uint64_t handled_access_net;
    uint64_t scoped;
};
struct ll_path_beneath_attr {
    uint64_t allowed_access;
    int32_t parent_fd;
} __attribute__((packed));
struct ll_net_port_attr {
    uint64_t allowed_access;
    uint64_t port;
};

// Always four arguments: landlock_add_rule's fourth is its flags, which
// must be 0, and syscall() would otherwise pass whatever the register held.
static long ll_syscall(long nr, long a, long b, long c) {
    return syscall(nr, a, b, c, 0L);
}

// The kernel's Landlock ABI, or 0 when it has none (not built, or not in
// the lsm= list: ENOSYS, EOPNOTSUPP).
static int landlock_abi(void) {
    long abi = ll_syscall(LL_CREATE_RULESET, 0, 0, LL_CREATE_RULESET_VERSION);
    return abi > 0 ? (int)abi : 0;
}

// A rule on `path` with `rights` (masked to what the ruleset handles, and to
// a file's rights when it is not a directory). A path that does not exist
// gets no rule. 0, or -1 with errno.
static int ll_rule(AetherSandboxKernel* k, const char* path, uint64_t rights) {
    rights &= k->landlock_fs;
    if (!rights) return 0;
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return (errno == ENOENT || errno == ENOTDIR || errno == EACCES) ? 0 : -1;
    struct stat st;
    if (fstat(fd, &st) != 0) { int e = errno; close(fd); errno = e; return -1; }
    if (!S_ISDIR(st.st_mode)) rights &= LL_FS_FILE;
    int rc = 0;
    if (rights) {
        struct ll_path_beneath_attr pb = { .allowed_access = rights, .parent_fd = fd };
        if (ll_syscall(LL_ADD_RULE, k->landlock_fd, LL_RULE_PATH_BENEATH, (long)&pb) != 0) rc = -1;
    }
    int e = errno;
    close(fd);
    errno = e;
    return rc;
}

static int is_dir(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// The rules for one fs-style pattern (see the mapping above). 0; 1 when it
// cannot be expressed (a suffix pattern); -1 with errno on a failure.
static int ll_pattern(AetherSandboxKernel* k, const char* pat, uint64_t rights) {
    size_t n = strlen(pat);
    if (n == 0) return 0;
    if (strcmp(pat, "*") == 0) return ll_rule(k, "/", rights);
    if (pat[0] == '*') return 1;
    if (pat[n - 1] != '*') {
        // Exact: a file. An exact directory is narrowed to nothing.
        if (is_dir(pat)) return 0;
        return ll_rule(k, pat, rights);
    }
    char prefix[PATH_MAX];
    if (n - 1 >= sizeof prefix) { errno = ENAMETOOLONG; return -1; }
    memcpy(prefix, pat, n - 1);
    prefix[n - 1] = '\0';
    if (strchr(prefix, '*')) return 1;
    size_t pn = n - 1;
    if (pn > 0 && prefix[pn - 1] == '/') {
        // "/dir/*": the directory and what is under it.
        if (pn > 1) prefix[pn - 1] = '\0';
        return is_dir(prefix) ? ll_rule(k, prefix, rights) : 0;
    }
    // "/dir/name*": each entry of /dir whose name starts with "name" now.
    char* slash = strrchr(prefix, '/');
    if (!slash) return 1;
    char dir[PATH_MAX];
    const char* name = slash + 1;
    size_t dn = (size_t)(slash - prefix);
    if (dn == 0) { dir[0] = '/'; dir[1] = '\0'; }
    else { memcpy(dir, prefix, dn); dir[dn] = '\0'; }
    size_t namelen = strlen(name);
    DIR* d = opendir(dir);
    if (!d) return 0;
    struct dirent* e;
    int rc = 0;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (strncmp(e->d_name, name, namelen) != 0) continue;
        char entry[PATH_MAX];
        int w = snprintf(entry, sizeof entry, "%s%s%s", dir, dn ? "/" : "", e->d_name);
        if (w < 0 || (size_t)w >= sizeof entry) continue;
        if (ll_rule(k, entry, rights) != 0) { rc = -1; break; }
    }
    closedir(d);
    return rc;
}

// An exec grant's command pattern ("python3", "/usr/bin/*", "echo *", "*"):
// its first word names the program.
static int ll_exec_pattern(AetherSandboxKernel* k, const char* pat) {
    char word[PATH_MAX];
    size_t n = strcspn(pat, " \t");
    if (n == 0 || n >= sizeof word) return n == 0 ? 0 : 1;
    memcpy(word, pat, n);
    word[n] = '\0';
    if (strcmp(word, "*") == 0 || strchr(word, '/')) {
        char canon[PATH_MAX];
        const char* use = word;
        // Resolve the directory part as fs grants are resolved.
        if (word[0] == '/' && word[n - 1] == '*' && n > 1) {
            char head[PATH_MAX];
            memcpy(head, word, n - 1);
            head[n - 1] = '\0';
            char* slash = strrchr(head, '/');
            if (slash) {
                *slash = '\0';
                char real[PATH_MAX];
                if (realpath(head[0] ? head : "/", real)) {
                    int w = snprintf(canon, sizeof canon, "%s/%s", strcmp(real, "/") ? real : "",
                                     slash + 1);
                    if (w > 0 && (size_t)w < sizeof canon) {
                        size_t cl = strlen(canon);
                        if (cl + 1 < sizeof canon) { canon[cl] = '*'; canon[cl + 1] = '\0'; use = canon; }
                    }
                }
            }
        }
        return ll_pattern(k, use, LL_EXEC);
    }
    char* path = aether_spawn_resolve(word);
    if (!path) return 0;
    int rc = ll_rule(k, path, LL_EXEC);
    free(path);
    return rc;
}

// Grant patterns into rules, before the ruleset is made, so a pattern that
// cannot be expressed stops the spawn before anything is open. The tcp
// grants also decide which net rights are handled: a pattern with no port
// (any port) or a listen grant cannot be expressed as ports, so that right
// is left to the preload and seccomp.
static void ll_net_handled(void* grant_list, uint64_t* net) {
    int n = list_size(grant_list);
    for (int i = 0; i + 1 < n; i += 2) {
        const char* cat = aether_string_data(list_get_raw(grant_list, i));
        const char* pat = aether_string_data(list_get_raw(grant_list, i + 1));
        if (!cat || !pat) continue;
        if (strcmp(cat, "tcp_listen") == 0) *net &= ~LL_NET_BIND_TCP;
        if (strcmp(cat, "tcp") == 0) {
            char host[256];
            if (aether_split_host_port(pat, host, sizeof host) < 0) *net &= ~LL_NET_CONNECT_TCP;
        }
    }
}

static int ll_grants(AetherSandboxKernel* k, void* grant_list, uint64_t net, char* err, size_t errsz) {
    int n = list_size(grant_list);
    for (int i = 0; i + 1 < n; i += 2) {
        const char* cat = aether_string_data(list_get_raw(grant_list, i));
        const char* pat = aether_string_data(list_get_raw(grant_list, i + 1));
        if (!cat || !pat) continue;
        int rc = 0;
        if (strcmp(cat, "fs_read") == 0) rc = ll_pattern(k, pat, LL_READ);
        else if (strcmp(cat, "fs_write") == 0) rc = ll_pattern(k, pat, LL_WRITE);
        else if (strcmp(cat, "fs") == 0) rc = ll_pattern(k, pat, LL_READ | LL_WRITE);
        else if (strcmp(cat, "exec") == 0) rc = ll_exec_pattern(k, pat);
        else if (strcmp(cat, "tcp") == 0 && (net & LL_NET_CONNECT_TCP)) {
            char host[256];
            int port = aether_split_host_port(pat, host, sizeof host);
            struct ll_net_port_attr np = { .allowed_access = LL_NET_CONNECT_TCP,
                                           .port = (uint64_t)port };
            if (port >= 0 &&
                ll_syscall(LL_ADD_RULE, k->landlock_fd, LL_RULE_NET_PORT, (long)&np) != 0)
                rc = -1;
        }
        if (rc == 1) {
            snprintf(err, errsz,
                     "the %s grant '%s' cannot be held by Landlock, which has no rule for a "
                     "name pattern (\"*.txt\") or a '*' inside a path; grant a directory "
                     "(\"/dir/*\"), a name prefix (\"/dir/name*\") or a file instead",
                     cat, pat);
            return 1;
        }
        if (rc < 0) {
            snprintf(err, errsz, "cannot add a Landlock rule for the %s grant '%s': %s",
                     cat, pat, strerror(errno));
            return -1;
        }
    }
    return 0;
}

// Does the list hold this exact (category, pattern) pair?
static int has_grant(void* grant_list, const char* category, const char* pattern) {
    int n = list_size(grant_list);
    for (int i = 0; i + 1 < n; i += 2) {
        const char* cat = aether_string_data(list_get_raw(grant_list, i));
        const char* pat = aether_string_data(list_get_raw(grant_list, i + 1));
        if (cat && pat && strcmp(cat, category) == 0 && strcmp(pat, pattern) == 0) return 1;
    }
    return 0;
}

static void kernel_warn(AetherSandboxKernel* k, const char* what) {
    static int warned_once = 0;
    k->warned = 1;
    if (warned_once) return;
    warned_once = 1;
    fprintf(stderr,
            "[aether] WARNING: spawn_sandboxed: %s, so the child runs with the preload and "
            "seccomp only and a raw syscall can open what the grants do not allow "
            "(docs/containment-sandbox.md, \"Landlock\"). sandbox.spawn_socket refuses to "
            "start a child here. This warning is printed once.\n", what);
}

// ---- prepare / program / apply / release ----

int aether_sandbox_kernel_prepare(AetherSandboxKernel* k, void* grant_list,
                                  int flags, const char* preload,
                                  const char* grants_path, char* err, size_t errsz) {
    memset(k, 0, sizeof *k);
    k->landlock_fd = -1;
    k->flags = flags;
    if (err && errsz) err[0] = '\0';
    char scratch[16];
    if (!err) { err = scratch; errsz = sizeof scratch; }

    // grant_all() is the equivalent of no sandbox.
    if (has_grant(grant_list, "*", "*")) return 0;

    // seccomp: built once the program is known (aether_sandbox_kernel_
    // program), since it names the pointer the child's execve passes.
    k->want_seccomp = 1;
    k->grant_fork = aether_sandbox_has_category(grant_list, "fork");
    k->grant_exec = aether_sandbox_has_category(grant_list, "exec");
    k->grant_net = aether_sandbox_has_category(grant_list, "tcp") ||
                   aether_sandbox_has_category(grant_list, "udp") ||
                   aether_sandbox_has_category(grant_list, "tcp_listen");
    {
        SbxPolicy probe;
        memset(&probe, 0, sizeof probe);
        struct sock_filter scratch_f[AETHER_SBX_FILTER_MAX];
        if (build_filter(scratch_f, AETHER_SBX_FILTER_MAX, &probe) < 0) {
            snprintf(err, errsz,
                     "no seccomp filter for this architecture, so the child cannot be "
                     "contained");
            return -1;
        }
    }

    // Landlock.
    int strict = (flags & AETHER_SBX_STRICT) != 0;
    int optional = has_grant(grant_list, "landlock", "optional");
    int abi = landlock_abi();
    if (abi <= 0) {
        if (strict && !optional) {
            snprintf(err, errsz,
                     "this kernel has no Landlock (Linux 5.13+, with landlock in its lsm= "
                     "list), so a raw syscall could open what the grants do not allow; "
                     "grant_without_landlock() runs the child with the preload and seccomp "
                     "only");
            return -1;
        }
        if (!strict) kernel_warn(k, "this kernel has no Landlock");
        return 0;
    }
    k->landlock_abi = abi;
    k->landlock_fs = LL_FS_V1;
    if (abi >= 2) k->landlock_fs |= LL_FS_REFER;
    if (abi >= 3) k->landlock_fs |= LL_FS_TRUNCATE;
    uint64_t net = abi >= 4 ? (LL_NET_BIND_TCP | LL_NET_CONNECT_TCP) : 0;
    ll_net_handled(grant_list, &net);
    struct ll_ruleset_attr attr = {
        .handled_access_fs = k->landlock_fs,
        .handled_access_net = net,
        .scoped = abi >= 6 ? (LL_SCOPE_ABSTRACT_UNIX_SOCKET | LL_SCOPE_SIGNAL) : 0,
    };
    size_t attr_size = abi >= 6 ? 24 : abi >= 4 ? 16 : 8;
    long fd = ll_syscall(LL_CREATE_RULESET, (long)&attr, (long)attr_size, 0);
    if (fd < 0) {
        snprintf(err, errsz, "cannot make a Landlock ruleset: %s", strerror(errno));
        return -1;
    }
    // Above the child's 0 to 3: the spawn puts the socket on 3, which would
    // close a ruleset there.
    int high = fcntl((int)fd, F_DUPFD_CLOEXEC, 10);
    close((int)fd);
    if (high < 0) {
        snprintf(err, errsz, "cannot move the Landlock ruleset: %s", strerror(errno));
        return -1;
    }
    k->landlock_fd = high;

    int rc = ll_grants(k, grant_list, net, err, errsz);
    if (rc == 0 && preload && ll_rule(k, preload, LL_FS_READ_FILE) != 0) {
        snprintf(err, errsz, "cannot add a Landlock rule for %s: %s", preload, strerror(errno));
        rc = -1;
    }
    if (rc == 0 && grants_path && ll_rule(k, grants_path, LL_FS_READ_FILE) != 0) {
        snprintf(err, errsz, "cannot add a Landlock rule for %s: %s", grants_path, strerror(errno));
        rc = -1;
    }
    if (rc != 0) {
        close(k->landlock_fd);
        k->landlock_fd = -1;
        if (strict) return -1;
        char why[600];
        snprintf(why, sizeof why, "%s", err);
        err[0] = '\0';
        kernel_warn(k, why);
    }
    return 0;
}

// The ELF interpreter (PT_INTERP) of `path` into `out`: 1, or 0 when it has
// none (static, not ELF).
static uint64_t elf_rd(const unsigned char* p, int bytes, int le) {
    uint64_t v = 0;
    for (int b = 0; b < bytes; b++)
        v |= (uint64_t)p[le ? b : bytes - 1 - b] << (8 * b);
    return v;
}

static int elf_interp(const char* path, char* out, size_t outsz) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    unsigned char h[64];
    int found = 0;
    if (pread(fd, h, sizeof h, 0) == (ssize_t)sizeof h && memcmp(h, "\177ELF", 4) == 0) {
        int is64 = h[4] == 2, le = h[5] == 1;
        uint64_t phoff = is64 ? elf_rd(h + 32, 8, le) : elf_rd(h + 28, 4, le);
        uint64_t phentsize = elf_rd(h + (is64 ? 54 : 42), 2, le);
        uint64_t phnum = elf_rd(h + (is64 ? 56 : 44), 2, le);
        for (uint64_t i = 0; i < phnum && i < 128 && !found; i++) {
            unsigned char ph[64];
            if (phentsize > sizeof ph || phentsize < (is64 ? 56u : 32u)) break;
            if (pread(fd, ph, (size_t)phentsize, (off_t)(phoff + i * phentsize)) != (ssize_t)phentsize)
                break;
            if (elf_rd(ph, 4, le) != 3 /* PT_INTERP */) continue;
            uint64_t off = is64 ? elf_rd(ph + 8, 8, le) : elf_rd(ph + 4, 4, le);
            uint64_t sz = is64 ? elf_rd(ph + 32, 8, le) : elf_rd(ph + 16, 4, le);
            if (sz == 0 || sz >= outsz) break;
            if (pread(fd, out, (size_t)sz, (off_t)off) == (ssize_t)sz) {
                out[sz] = '\0';
                found = out[0] == '/';
            }
        }
    }
    close(fd);
    return found;
}

// The program at `path`, its interpreter and the loader's floor. `depth`
// stops a #! chain.
static int ll_program(AetherSandboxKernel* k, const char* path, const char* sh_path, int depth) {
    if (ll_rule(k, path, LL_EXEC) != 0) return -1;
    char interp[PATH_MAX];
    if (elf_interp(path, interp, sizeof interp)) {
        if (ll_rule(k, interp, LL_EXEC) != 0) return -1;
        // The loader's floor: the libraries beside the interpreter, read
        // only and not listed (the multiarch lib directory on Debian and
        // Ubuntu, /usr/lib on Arch and Fedora).
        char real[PATH_MAX];
        if (realpath(interp, real)) {
            char* slash = strrchr(real, '/');
            if (slash && slash != real) {
                *slash = '\0';
                if (ll_rule(k, real, LL_FS_READ_FILE) != 0) return -1;
            }
        }
        return 0;
    }
    // A #! script: its interpreter.
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    char head[PATH_MAX + 2];
    ssize_t n = fd >= 0 ? pread(fd, head, sizeof head - 1, 0) : -1;
    if (fd >= 0) close(fd);
    if (n > 2 && head[0] == '#' && head[1] == '!') {
        head[n] = '\0';
        char* p = head + 2;
        while (*p == ' ' || *p == '\t') p++;
        p[strcspn(p, " \t\r\n")] = '\0';
        if (p[0] == '/' && depth < 4) return ll_program(k, p, sh_path, depth + 1);
        return 0;
    }
    if (n >= 4 && memcmp(head, "\177ELF", 4) == 0) return 0;   // static ELF
    // Neither: the spawn runs it under /bin/sh.
    if (sh_path && depth == 0) return ll_program(k, sh_path, NULL, depth + 1);
    return 0;
}

int aether_sandbox_kernel_exec_fenced(const AetherSandboxKernel* k) {
    return k->want_seccomp && !k->grant_exec;
}

static int kernel_filter(AetherSandboxKernel* k, const char* path, const char* sh_path,
                         char* err, size_t errsz) {
    if (!k->want_seccomp) return 0;
    SbxPolicy p;
    memset(&p, 0, sizeof p);
    p.fork = k->grant_fork;
    p.exec = k->grant_exec;
    p.net = k->grant_net;
    if (path) p.ptrs[p.nptrs++] = (uint64_t)(uintptr_t)path;
    if (sh_path) p.ptrs[p.nptrs++] = (uint64_t)(uintptr_t)sh_path;
    int n = build_filter(k->filter, AETHER_SBX_FILTER_MAX, &p);
    if (n < 0) {
        snprintf(err, errsz, "cannot build the seccomp filter for this architecture");
        return -1;
    }
    k->seccomp = 1;
    k->filter_len = (unsigned short)n;
    return 0;
}

int aether_sandbox_kernel_program(AetherSandboxKernel* k, const char* path,
                                  const char* sh_path, char* err, size_t errsz) {
    if (kernel_filter(k, path, sh_path, err, errsz) != 0) return -1;
    if (k->landlock_fd < 0) return 0;
    static const char* const floor_read[] = {
        "/etc/ld.so.cache",       // the loader's index
        "/usr/lib/locale",        // glibc's locale archive
        "/dev/urandom",
    };
    int rc = ll_program(k, path, sh_path, 0);
    for (size_t i = 0; rc == 0 && i < sizeof floor_read / sizeof floor_read[0]; i++)
        rc = ll_rule(k, floor_read[i], LL_FS_READ_FILE);
    if (rc == 0) rc = ll_rule(k, "/dev/null", LL_FS_READ_FILE | LL_FS_WRITE_FILE);
    if (rc != 0) {
        snprintf(err, errsz, "cannot add the Landlock rules to start %s: %s", path, strerror(errno));
        if (k->flags & AETHER_SBX_STRICT) return -1;
        close(k->landlock_fd);
        k->landlock_fd = -1;
        kernel_warn(k, err);
        err[0] = '\0';
    }
    return 0;
}

int aether_sandbox_kernel_apply(const AetherSandboxKernel* k) {
    if (!k->seccomp && k->landlock_fd < 0) return 0;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return k->landlock_fd >= 0 ? AETHER_SBX_STEP_LANDLOCK : AETHER_SBX_STEP_SECCOMP;
    if (k->landlock_fd >= 0 && ll_syscall(LL_RESTRICT_SELF, k->landlock_fd, 0, 0) != 0)
        return AETHER_SBX_STEP_LANDLOCK;
    if (k->seccomp) {
        struct sock_fprog prog = { .len = k->filter_len,
                                   .filter = (struct sock_filter*)k->filter };
        if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) return AETHER_SBX_STEP_SECCOMP;
    }
    return 0;
}

void aether_sandbox_kernel_release(AetherSandboxKernel* k) {
    if (k->landlock_fd >= 0) close(k->landlock_fd);
    k->landlock_fd = -1;
}

const char* aether_sandbox_kernel_failure(int step, int e, char* buf, size_t bufsz) {
    const char* what = step == AETHER_SBX_STEP_SECCOMP
        ? "cannot install the seccomp filter (needs Linux 3.5+ with "
          "PR_SET_NO_NEW_PRIVS and PR_SET_SECCOMP)"
        : step == AETHER_SBX_STEP_LANDLOCK
        ? "cannot apply the Landlock ruleset"
        : "cannot apply the kernel layer";
    snprintf(buf, bufsz, "%s: %s", what, strerror(e));
    return buf;
}

#endif // __linux__ && !__ANDROID__
