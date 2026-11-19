#!/bin/sh
# The preload's syscall() interposer: with nothing in force it forwards
# every call; under a policy (a grant file, or an in-process level on the
# calling thread) it is default-deny, forwarding only an allowlist of calls
# that reach nothing outside the process.
#
# It used to answer every raw syscall() ENOSYS when nothing was enforced,
# and EPERM under a grant file, futexes included, so with
# libaether_sandbox.so preloaded GLib's futex waits returned at once and its
# threads spun (an idle GTK4 program burned 28 s of CPU in 12 s), and the
# Aether scheduler's sched_getaffinity and membarrier failed. Now:
#   - futex, sched_getaffinity, membarrier, gettid, a six-argument mmap and
#     a signal to this process reach the kernel, with or without a policy;
#   - under a policy a raw openat, unlinkat, renameat, sendto, kill of
#     another process and an unknown number are refused and logged, unless a
#     native grant names the call; with nothing in force they are forwarded;
#   - libc's unlink(), which this library has no wrapper for, gets the
#     kernel's answer whatever the policy.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LIB="$ROOT/build/libaether_sandbox.so"

case "$(uname -s 2>/dev/null)" in
    Linux) ;;
    *)
        echo "  [SKIP] sandbox_preload_syscall: the probe is Linux syscall numbers"
        exit 0 ;;
esac
if [ ! -f "$LIB" ]; then
    echo "  [SKIP] sandbox_preload_syscall: preload library not built"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
if ! ${CC:-cc} -std=c11 -Wall -Wextra -Werror -rdynamic \
        "$SCRIPT_DIR/probe.c" -o "$TMP/probe"; then
    echo "  [FAIL] sandbox_preload_syscall: probe did not compile"
    exit 1
fi

# Two pages, the second all 'B' (the mmap check maps it at its offset).
page=$(getconf PAGESIZE 2>/dev/null || echo 4096)
{ head -c "$page" /dev/zero; head -c "$page" /dev/zero | tr '\0' 'B'; } > "$TMP/data.bin"
DATA="$TMP/data.bin"

printf 'fs_read:%s\nfs_write:%s/*\n' "$DATA" "$TMP" > "$TMP/read.grants"
printf 'fs_read:%s\nfs_write:%s/*\nnative:syscall:openat\nnative:syscall:unlinkat\n' "$DATA" "$TMP" > "$TMP/read-native.grants"
printf '*:*\n' > "$TMP/all.grants"
fail=0

FORWARDED="futex_wait futex_wake sched_getaffinity gettid mmap_six_args raw_kill_self raw_tgkill_self libc_unlink"
POLICED="raw_openat raw_unlinkat raw_renameat raw_kill_other raw_sendto raw_unknown"

# The probe with no preload: what the kernel answers, the reference for the
# checks the preload must forward.
if ! "$TMP/probe" none "$DATA" > "$TMP/base.out" 2> "$TMP/base.err"; then
    echo "  [FAIL] probe without the preload:"
    cat "$TMP/base.out" "$TMP/base.err"
    exit 1
fi
for check in $FORWARDED $POLICED; do
    if ! grep -qx "$check ok" "$TMP/base.out"; then
        echo "  [FAIL] without the preload, $check did not pass:"
        cat "$TMP/base.out" "$TMP/base.err"
        exit 1
    fi
done
grep '^membarrier ' "$TMP/base.out" > "$TMP/base.membarrier"

# run_case LABEL LEVEL GRANTS ALLOWED...
#   GRANTS: a grant file, or "" for none. ALLOWED: the policed checks that
#   must pass (all others must be refused EPERM and logged).
run_case() {
    label="$1"
    level="$2"
    grants="$3"
    shift 3
    allowed=" $* "
    if [ -n "$grants" ]; then
        LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$grants" AETHER_SANDBOX_LOG=stderr \
            "$TMP/probe" "$level" "$DATA" > "$TMP/out" 2> "$TMP/err"
    else
        LD_PRELOAD="$LIB" AETHER_SANDBOX_LOG=stderr \
            "$TMP/probe" "$level" "$DATA" > "$TMP/out" 2> "$TMP/err"
    fi
    rc=$?
    bad=""
    [ "$rc" -eq 0 ] || bad="exit $rc"
    for check in $FORWARDED; do
        grep -qx "$check ok" "$TMP/out" || bad="$bad; $(grep "^$check " "$TMP/out")"
    done
    [ "$(grep '^membarrier ' "$TMP/out")" = "$(cat "$TMP/base.membarrier")" ] ||
        bad="$bad; $(grep '^membarrier ' "$TMP/out") (without the preload: $(cat "$TMP/base.membarrier"))"
    for check in $POLICED; do
        case "$allowed" in
            *" $check "*) grep -qx "$check ok" "$TMP/out" ||
                              bad="$bad; $(grep "^$check " "$TMP/out"), expected ok" ;;
            *) grep -qx "$check EPERM" "$TMP/out" ||
                   bad="$bad; $(grep "^$check " "$TMP/out"), expected EPERM" ;;
        esac
    done
    # Each refusal is logged under the call's name (the number for one the
    # preload does not name).
    for pair in raw_openat:openat raw_unlinkat:unlinkat raw_renameat:renameat \
                raw_kill_other:kill raw_sendto:sendto raw_unknown:4000; do
        check="${pair%%:*}"
        grep -qx "$check EPERM" "$TMP/out" || continue
        grep -q "^AETHER_DENIED: native syscall:${pair#*:}\$" "$TMP/err" ||
            bad="$bad; the refused $check was not logged as syscall:${pair#*:}"
    done
    if [ -z "$bad" ]; then
        echo "  [PASS] $label"
    else
        echo "  [FAIL] $label:${bad#;}"
        sed 's/^/           /' "$TMP/out" "$TMP/err"
        fail=1
    fi
}

ALL="$POLICED"
run_case "no policy: every raw syscall forwarded" none "" $ALL
run_case "grant file: futex & co forwarded, the rest refused" none "$TMP/read.grants"
run_case "grant file naming syscall:openat and :unlinkat: those two allowed" none \
    "$TMP/read-native.grants" raw_openat raw_unlinkat
run_case "grant file *:*: everything allowed" none "$TMP/all.grants" $ALL
run_case "in-process level: futex & co forwarded, the rest refused" deny ""

echo ""
echo "sandbox_preload_syscall: $([ "$fail" -eq 0 ] && echo PASS || echo FAIL)"
[ "$fail" -eq 0 ]
