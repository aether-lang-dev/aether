#!/bin/sh
# Regression for issue #668: the LD_PRELOAD libc-symbol fence on
# fork/vfork/clone is bypassed by callers that issue the underlying
# syscall directly — including glibc's own `__vfork` (an inline
# `syscall` instruction with no libc symbol indirection) and any
# program calling `syscall(SYS_clone3, ...)` via libc's syscall()
# wrapper that is itself bypassable on some glibc builds.
#
# Fix: spawn_sandboxed installs a seccomp-bpf filter on the child side
# (post-fork, pre-exec) that traps clone/clone3/fork/vfork with EPERM
# when `fork:*` is not granted. Kernel-level enforcement, immune to
# how the syscall is invoked.
#
# This test:
#   1. Builds two probe binaries — one calls libc's vfork() (inline
#      __vfork syscall instruction); the other calls
#      syscall(SYS_clone3, ...) directly. Both successfully fork their
#      child OUTSIDE the sandbox.
#   2. Runs each via `spawn_sandboxed` with no `fork` grant. Both must
#      now fail (the syscall returns EPERM and the probe exits non-zero).
#   3. Runs each via `spawn_sandboxed` WITH `fork:*` granted. Both must
#      succeed — the fence is opt-in via the grant grammar.
#
# These are the two specific bypass paths called out in the issue. A
# pass here proves the kernel-level fence is installed and effective.
#
# #3178 changed what the fence answers: a clone with CLONE_THREAD (a
# thread, no new process) passes, and clone3, whose flags a filter cannot
# read, gets ENOSYS so libc falls back to clone. Two cases after the vfork
# ones check both: pthread_create works without a fork grant, and a raw
# clone3 instruction gets ENOSYS.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] seccomp-bpf is Linux-only"
        exit 0 ;;
    Linux) ;;
    *)
        echo "  [SKIP] seccomp-bpf is Linux-only (this is $(uname -s))"
        exit 0 ;;
esac

# The probes below are x86_64 C (glibc's inline-syscall vfork, raw clone3),
# and the i386 probe needs an x86_64 kernel. The fence itself covers
# x86_64, aarch64, riscv64 and loongarch64; the per-arch checks at the end
# verify the others without hardware.
case "$(uname -m 2>/dev/null)" in
    x86_64|amd64) ;;
    *)
        echo "  [SKIP] the probes here are x86_64 programs (this is $(uname -m))"
        exit 0 ;;
esac

AE="$ROOT/build/ae"
if [ ! -x "$AE" ]; then
    echo "  [SKIP] $AE not built (run make first)"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

# find_preload_path in aether_spawn_sandboxed.c looks next to the
# launcher binary (and at ../build/libaether_sandbox.so). Put the .so
# next to where we'll build the launcher so spawn_sandboxed can find it.
if [ -f "$ROOT/build/libaether_sandbox.so" ]; then
    cp "$ROOT/build/libaether_sandbox.so" "$TMPDIR/libaether_sandbox.so"
else
    echo "  [SKIP] $ROOT/build/libaether_sandbox.so not built"
    exit 0
fi

# Probe 1: libc vfork(). glibc's __vfork on x86_64 is an inline syscall
# instruction in libc's text — LD_PRELOAD CANNOT see it.
cat > "$TMPDIR/probe_vfork.c" <<'C'
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
int main(void) {
    pid_t p = vfork();
    if (p < 0) { perror("vfork"); return 1; }
    if (p == 0) { _exit(42); }
    int s; waitpid(p, &s, 0);
    if (WIFEXITED(s) && WEXITSTATUS(s) == 42) {
        printf("vfork-ok\n");
        return 0;
    }
    return 2;
}
C
cc -o "$TMPDIR/probe_vfork" "$TMPDIR/probe_vfork.c"

# Probe 2: raw clone3 via syscall(). libc's syscall() wrapper IS
# interposable via LD_PRELOAD, but a program that uses the inline asm
# syscall instruction would not be — both should be caught by seccomp.
cat > "$TMPDIR/probe_clone3.c" <<'C'
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/sched.h>
#include <unistd.h>
int main(void) {
    struct clone_args args = {0};
    args.flags = 0;
    args.exit_signal = SIGCHLD;
    long p = syscall(SYS_clone3, &args, sizeof(args));
    if (p < 0) { perror("clone3"); return 1; }
    if (p == 0) { _exit(43); }
    int s; waitpid((pid_t)p, &s, 0);
    if (WIFEXITED(s) && WEXITSTATUS(s) == 43) {
        printf("clone3-ok\n");
        return 0;
    }
    return 2;
}
C
cc -o "$TMPDIR/probe_clone3" "$TMPDIR/probe_clone3.c"

# Baseline: both probes succeed outside the sandbox.
if ! "$TMPDIR/probe_vfork" > "$TMPDIR/vfork.out" 2>&1; then
    echo "  [FAIL] baseline probe_vfork outside sandbox failed:"
    cat "$TMPDIR/vfork.out"
    exit 1
fi
if ! grep -q '^vfork-ok$' "$TMPDIR/vfork.out"; then
    echo "  [FAIL] baseline probe_vfork did not print vfork-ok:"
    cat "$TMPDIR/vfork.out"
    exit 1
fi
if ! "$TMPDIR/probe_clone3" > "$TMPDIR/clone3.out" 2>&1; then
    # No clone3 at all (a kernel before 5.3, or an x86_64 emulator such as
    # Rosetta for Linux, which also hands the kernel translated aarch64
    # syscalls, so an x86_64 seccomp filter never matches): there is no
    # fence to exercise here, rather than a fence that failed.
    if grep -q 'Function not implemented' "$TMPDIR/clone3.out"; then
        echo "  [SKIP] clone3 is not implemented here (old kernel, or an x86_64 emulator)"
        exit 0
    fi
    echo "  [FAIL] baseline probe_clone3 outside sandbox failed:"
    cat "$TMPDIR/clone3.out"
    exit 1
fi
if ! grep -q '^clone3-ok$' "$TMPDIR/clone3.out"; then
    echo "  [FAIL] baseline probe_clone3 did not print clone3-ok:"
    cat "$TMPDIR/clone3.out"
    exit 1
fi

# Launcher: spawn_sandboxed a probe with the given grants. Returns the
# spawned child's exit code via println("rc=N").
make_launcher() {
    grant_block="$1"
    probe_path="$2"
    out_ae="$3"
    cat > "$out_ae" <<AE
import std.list
extern println(s: string)
main() {
    g = list.new()
${grant_block}
    rc = spawn_sandboxed(g, "${probe_path}", "")
    println("rc=\${rc}")
}
AE
}

# Grants WITHOUT fork.
GRANTS_NO_FORK='    list.add(g,"exec"); list.add(g,"/*")
    list.add(g,"fs_read"); list.add(g,"/*")
    list.add(g,"fs_write"); list.add(g,"/*")
    list.add(g,"env"); list.add(g,"*")
    list.add(g,"native"); list.add(g,"*")'

# Grants WITH fork.
GRANTS_WITH_FORK='    list.add(g,"fork"); list.add(g,"*")
    list.add(g,"exec"); list.add(g,"/*")
    list.add(g,"fs_read"); list.add(g,"/*")
    list.add(g,"fs_write"); list.add(g,"/*")
    list.add(g,"env"); list.add(g,"*")
    list.add(g,"native"); list.add(g,"*")'

run_case() {
    label="$1"
    grants="$2"
    probe="$3"
    expect_inside="$4"   # 0 if the probe should succeed inside the sandbox

    launcher_ae="$TMPDIR/${label}.ae"
    launcher_bin="$TMPDIR/${label}"
    make_launcher "$grants" "$probe" "$launcher_ae"
    if ! "$AE" build "$launcher_ae" -o "$launcher_bin" > "$TMPDIR/${label}.build.log" 2>&1; then
        echo "  [FAIL] $label launcher build failed:"
        tail -20 "$TMPDIR/${label}.build.log"
        exit 1
    fi
    "$launcher_bin" > "$TMPDIR/${label}.out" 2>&1 || true
    inside_rc=$(grep '^rc=' "$TMPDIR/${label}.out" | tail -1 | sed 's/^rc=//')
    if [ -z "$inside_rc" ]; then
        echo "  [FAIL] $label produced no rc= line:"
        cat "$TMPDIR/${label}.out"
        exit 1
    fi

    if [ "$expect_inside" = "0" ]; then
        if [ "$inside_rc" != "0" ]; then
            echo "  [FAIL] $label: expected probe to succeed inside sandbox (rc=0), got rc=$inside_rc:"
            cat "$TMPDIR/${label}.out"
            exit 1
        fi
    else
        if [ "$inside_rc" = "0" ]; then
            echo "  [FAIL] $label: expected probe to be DENIED inside sandbox, but it succeeded (rc=0):"
            cat "$TMPDIR/${label}.out"
            exit 1
        fi
    fi
}

# Without fork grant: both probes must be denied. These are the
# load-bearing assertions for issue #668 — the two bypass paths
# (libc vfork = inline syscall instruction, raw syscall(SYS_clone3))
# both fail at the new kernel-level fence.
run_case  vfork_no_fork    "$GRANTS_NO_FORK"   "$TMPDIR/probe_vfork"   denied
run_case  clone3_no_fork   "$GRANTS_NO_FORK"   "$TMPDIR/probe_clone3"  denied

# With fork grant: libc vfork must succeed (the seccomp filter is not
# installed at all when fork:* is in the grant list, so glibc's
# inline-syscall vfork goes through). The clone3-via-libc-syscall
# probe is NOT tested with fork granted because the preload's existing
# blanket `syscall()` wrapper denies ALL libc-syscall calls regardless
# of category — that pre-dates this fix and is a separate property.
# A probe that issued the clone3 instruction *directly* (asm) would
# succeed here, mirroring vfork's case.
run_case  vfork_with_fork  "$GRANTS_WITH_FORK" "$TMPDIR/probe_vfork"   0

echo "  [PASS] seccomp fence blocks raw clone3 + libc vfork when fork:* not granted (issue #668)"

# A thread is no new process, and passes the fence (#3178). The fence used
# to refuse every clone, so pthread_create failed in any child that starts
# a thread: an Aether program with actors, Python with threads, a JVM.
# glibc tries clone3 first; the fence answers it ENOSYS, as a kernel
# without clone3 does, and glibc falls back to clone, whose CLONE_THREAD
# flag the filter reads.
cat > "$TMPDIR/probe_thread.c" <<'C'
#include <pthread.h>
#include <stdio.h>
static void* work(void* p) { *(int*)p = 7; return NULL; }
int main(void) {
    pthread_t t;
    int v = 0;
    int rc = pthread_create(&t, NULL, work, &v);
    if (rc != 0) { fprintf(stderr, "pthread_create: %d\n", rc); return 1; }
    pthread_join(t, NULL);
    if (v != 7) return 2;
    printf("thread-ok\n");
    return 0;
}
C
cc -pthread -o "$TMPDIR/probe_thread" "$TMPDIR/probe_thread.c"
run_case  thread_no_fork   "$GRANTS_NO_FORK"   "$TMPDIR/probe_thread"  0
echo "  [PASS] a thread starts inside the fence (#3178)"

# clone3 issued as a raw syscall instruction, past libc and the preload,
# gets ENOSYS: the answer that makes libc fall back to clone. A child the
# call might make exits at once with 0, which this reports as created.
cat > "$TMPDIR/probe_clone3_raw.c" <<'C'
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
struct args3 { uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls; };
static long raw_clone3(struct args3* a, unsigned long size) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(435L), "D"(a), "S"(size) : "rcx", "r11", "memory");
    return ret;
}
int main(void) {
    struct args3 a;
    memset(&a, 0, sizeof(a));
    a.exit_signal = 17;   /* SIGCHLD */
    long r = raw_clone3(&a, sizeof(a));
    if (r == 0) _exit(0);
    if (r > 0) { int st; waitpid((pid_t)r, &st, 0); printf("clone3: created a process\n"); return 0; }
    printf("clone3: errno %ld\n", -r);
    return 1;
}
C
cc -o "$TMPDIR/probe_clone3_raw" "$TMPDIR/probe_clone3_raw.c"
run_case  clone3_raw_no_fork  "$GRANTS_NO_FORK"  "$TMPDIR/probe_clone3_raw"  denied
if grep -q "^clone3: errno 38$" "$TMPDIR/clone3_raw_no_fork.out"; then
    echo "  [PASS] a raw clone3 gets ENOSYS inside the fence (#3178)"
else
    echo "  [FAIL] a raw clone3 inside the fence: expected errno 38 (ENOSYS):"
    cat "$TMPDIR/clone3_raw_no_fork.out"
    exit 1
fi

# ---- Other ABIs and architectures (needs zig; skipped without it) ----
#
# A seccomp filter sees the ABI a syscall came in through. The fence used to
# check x86_64 numbers and allow every other ABI, so a static 32-bit binary
# (i386 ABI, which LD_PRELOAD cannot reach either) forked freely inside the
# sandbox. Now each ABI the kernel accepts is trapped with its own numbers,
# and any other ABI is killed.
if command -v zig > /dev/null 2>&1; then
    cat > "$TMPDIR/probe_fork32.c" <<'C'
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
int main(void) {
    pid_t p = fork();
    if (p < 0) { perror("fork"); return 1; }
    if (p == 0) { _exit(44); }
    int s; waitpid(p, &s, 0);
    if (WIFEXITED(s) && WEXITSTATUS(s) == 44) { printf("fork32-ok\n"); return 0; }
    return 2;
}
C
    if zig cc -target x86-linux-musl -static -O1 "$TMPDIR/probe_fork32.c" -o "$TMPDIR/probe_fork32" \
            > "$TMPDIR/zig32.log" 2>&1 && "$TMPDIR/probe_fork32" > /dev/null 2>&1; then
        run_case  fork32_no_fork    "$GRANTS_NO_FORK"   "$TMPDIR/probe_fork32"  denied
        run_case  fork32_with_fork  "$GRANTS_WITH_FORK" "$TMPDIR/probe_fork32"  0
        echo "  [PASS] a static i386 binary cannot fork inside the sandbox (compat ABI fenced)"
    else
        echo "  [SKIP] i386 probe: this kernel does not run 32-bit binaries, or zig could not build one"
    fi

    # The filter's literals are each ABI's real numbers, from the kernel
    # headers zig ships for that architecture.
    SRC="$ROOT/runtime/sandbox/aether_sandbox_kernel_linux.c"
    # The tables: <name>_nr[SC_COUNT] = { ... } in the order of the
    # "SBX_NAMES:" comment, -1 for a call the ABI does not have; x32's own
    # numbers in x32_nr, in the order of "SBX_X32_NAMES:".
    nums_of() { sed -n "s/.*[^a-z0-9_]$1\[[A-Z_0-9]*\] *= *{ *\([-0-9, ]*\)}.*/\1/p" "$SRC" | tr -d ' '; }
    names_of() { sed -n "s|^// $1: ||p" "$SRC"; }
    hdr_of() {   # <target> <names...>: the numbers of those syscalls (-1 if none), comma-joined
        t="$1"; shift
        defs=$(echo '#include <sys/syscall.h>' | zig cc -target "$t" -E -dM -x c - 2>/dev/null)
        out=""
        for nm in "$@"; do
            v=$(printf '%s\n' "$defs" | sed -n -e "s/^#define SYS_$nm \([0-9]*\)$/\1/p" \
                -e "s/^#define SYS_$nm (0x40000000 + \([0-9]*\))$/\1/p")
            [ -n "$v" ] || v=-1
            out="$out${out:+,}$v"
        done
        echo "$out"
    }
    check_nums() {   # <label> <array> <target> <names...>
        label="$1"; arr="$2"; t="$3"; shift 3
        want=$(hdr_of "$t" "$@"); got=$(nums_of "$arr")
        if [ -n "$want" ] && [ "$want" = "$got" ]; then
            echo "  [PASS] $label syscall numbers match $t's headers"
        else
            echo "  [FAIL] $label: the filter has '$got', $t's headers say '$want'"
            exit 1
        fi
    }
    NAMES=$(names_of SBX_NAMES)
    X32NAMES=$(names_of SBX_X32_NAMES)
    [ -n "$NAMES" ] && [ -n "$X32NAMES" ] || { echo "  [FAIL] no SBX_NAMES in $SRC"; exit 1; }
    # shellcheck disable=SC2086
    {
    check_nums "x86_64"      x86_64_nr  x86_64-linux-musl      $NAMES
    check_nums "x32"         x32_nr     x86_64-linux-muslx32   $X32NAMES
    check_nums "i386"        i386_nr    x86-linux-musl         $NAMES
    check_nums "32-bit ARM"  arm_nr     arm-linux-musleabihf   $NAMES
    check_nums "aarch64"     generic_nr aarch64-linux-musl     $NAMES
    check_nums "riscv64"     generic_nr riscv64-linux-musl     $NAMES
    check_nums "loongarch64" generic_nr loongarch64-linux-musl $NAMES
    }

    # The fence compiles for every architecture it claims.
    for t in aarch64-linux-musl riscv64-linux-musl loongarch64-linux-musl; do
        if zig cc -target "$t" -c -I"$ROOT/runtime" -I"$ROOT/runtime/utils" -I"$ROOT/runtime/actors" \
                -I"$ROOT/runtime/scheduler" -I"$ROOT/runtime/memory" -I"$ROOT/runtime/config" \
                -I"$ROOT/std" -I"$ROOT/std/string" "$SRC" -o "$TMPDIR/fence-$t.o" > "$TMPDIR/fence-$t.log" 2>&1; then
            echo "  [PASS] the fence builds for $t"
        else
            echo "  [FAIL] the fence does not build for $t"; head -5 "$TMPDIR/fence-$t.log"
            exit 1
        fi
    done
else
    echo "  [SKIP] i386 probe and per-arch checks: no zig"
fi
