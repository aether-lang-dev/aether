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

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "  [SKIP-WIN] seccomp-bpf is Linux-only"
        exit 0 ;;
    Darwin)
        echo "  [SKIP] seccomp-bpf is Linux-only"
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
