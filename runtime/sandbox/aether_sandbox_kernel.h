// aether_sandbox_kernel.h: the kernel layer of a sandboxed spawn (Linux).
//
// The LD_PRELOAD library sees only the calls that go through libc. What a
// child does past libc (a raw syscall instruction, a static binary, ctypes
// reaching libc's own syscall()) is held by the kernel, with what this file
// sets up: a seccomp-bpf filter (docs/containment-sandbox.md, "Kernel
// layer").
//
// Both sandboxed spawns use it: spawn_sandboxed (runtime/sandbox/
// spawn_sandboxed_linux.c) and sandbox.spawn_socket. The work is split so a
// child of a threaded parent calls only async-signal-safe functions between
// fork and exec (#3200):
//   - aether_sandbox_kernel_prepare, in the parent before the fork, reads
//     the grant list and builds everything: the filter, as data;
//   - aether_sandbox_kernel_apply, in the child before exec, installs it:
//     a few prctl calls, no malloc, no stdio;
//   - aether_sandbox_kernel_release, in the parent after the fork.
#ifndef AETHER_SANDBOX_KERNEL_H
#define AETHER_SANDBOX_KERNEL_H

#if defined(__linux__) && !defined(__ANDROID__)

#include <stddef.h>
#include <linux/filter.h>

#define AETHER_SBX_FILTER_MAX 512

typedef struct AetherSandboxKernel {
    int seccomp;                                  // 1: install `filter`
    unsigned short filter_len;
    struct sock_filter filter[AETHER_SBX_FILTER_MAX];
    int landlock_fd;                              // the ruleset, or -1: no Landlock
    int landlock_abi;                             // the kernel's ABI, 0 when none
    unsigned long long landlock_fs;               // the fs rights it handles
    int flags;
    int warned;                                   // a warning was printed
} AetherSandboxKernel;

// What a child that could not apply the layer reports, with errno.
enum {
    AETHER_SBX_STEP_SECCOMP = 1,
    AETHER_SBX_STEP_LANDLOCK = 2,
};

// How strict prepare is (the two spawns differ only here).
//   AETHER_SBX_STRICT: anything the kernel layer cannot hold is an error,
//     and the spawn starts nothing (sandbox.spawn_socket).
//   0: the child still starts, with what can be held, and a warning on
//     stderr says what is missing (spawn_sandboxed, whose callers predate
//     the kernel layer).
#define AETHER_SBX_STRICT 1

// Parent, before the fork. 0 when `k` is ready for aether_sandbox_kernel_
// program; -1 when the spawn must not start, with the reason in `err`.
// `preload` and `grants_path` (either may be NULL) are files the child must
// read before its own code runs: the preload library, and spawn_sandboxed's
// shared-memory object.
int aether_sandbox_kernel_prepare(AetherSandboxKernel* k, void* grant_list,
                                  int flags, const char* preload,
                                  const char* grants_path, char* err, size_t errsz);

// Parent, once the program is resolved: what the child needs to exec it
// (the program, its ELF interpreter, the interpreter of a #! script, and
// /bin/sh at `sh_path` for a file that is neither), and the loader's floor.
// 0, or -1 with the reason in `err`.
int aether_sandbox_kernel_program(AetherSandboxKernel* k, const char* path,
                                  const char* sh_path, char* err, size_t errsz);

// Child, before exec: async-signal-safe. 0, or a nonzero
// AETHER_SBX_STEP_* with errno set.
int aether_sandbox_kernel_apply(const AetherSandboxKernel* k);

// Parent, after the fork.
void aether_sandbox_kernel_release(AetherSandboxKernel* k);

// A child's report as words: "<what failed>: <strerror>" in `buf`.
const char* aether_sandbox_kernel_failure(int step, int err, char* buf, size_t bufsz);

// Is `category` granted anywhere in the list? A (*, *) grant grants it too.
int aether_sandbox_has_category(void* grant_list, const char* category);

#endif // __linux__ && !__ANDROID__
#endif // AETHER_SANDBOX_KERNEL_H
