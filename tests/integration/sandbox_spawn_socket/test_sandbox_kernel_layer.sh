#!/bin/sh
# The kernel layer under sandbox.spawn_socket and spawn_sandboxed (Linux):
# what a child does past libc, and so past LD_PRELOAD, is held by the kernel.
# probe.c issues the syscall instruction itself.
#
#   Landlock: a raw openat of a file the grants do not cover is refused; one
#   they cover is allowed. A grant Landlock cannot express is refused, not
#   widened. On a kernel with no Landlock (simulated by nolandlock.c, which
#   makes landlock_create_ruleset fail with ENOSYS) spawn_socket refuses
#   unless the host passes grant_without_landlock(), and spawn_sandboxed
#   still runs its child, with a warning.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ "$(uname -s 2>/dev/null)" != Linux ]; then
    echo "  [SKIP] sandbox_kernel_layer: Landlock and seccomp are Linux's"
    exit 0
fi
case "$(uname -m)" in
    x86_64|aarch64) ;;
    *) echo "  [SKIP] sandbox_kernel_layer: probe.c's raw syscalls are x86_64 and aarch64"; exit 0 ;;
esac
if [ ! -x "$AE" ] || [ ! -f "$ROOT/build/libaether_sandbox.so" ]; then
    echo "  [SKIP] sandbox_kernel_layer: ae or libaether_sandbox.so not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT
export AETHER_HOME="$ROOT"

fail() {
    echo "  [FAIL] sandbox_kernel_layer: $1"
    sed 's/^/    /' "$2"
    exit 1
}

for c in probe nolandlock; do
    ${CC:-cc} -O1 -o "$TMP/$c" "$SCRIPT_DIR/$c.c" > "$TMP/cc.log" 2>&1 || fail "cannot build $c.c" "$TMP/cc.log"
done
for a in host sandboxed; do
    "$AE" build "$SCRIPT_DIR/$a.ae" -o "$TMP/$a" > "$TMP/build.log" 2>&1 || fail "cannot build $a.ae" "$TMP/build.log"
done
echo "greeting=from the fixture" > "$TMP/fixture.txt"

# Does this kernel have Landlock? (The ABI query: landlock_create_ruleset
# with LANDLOCK_CREATE_RULESET_VERSION.)
if python3 -c 'import ctypes,sys; sys.exit(0 if ctypes.CDLL(None).syscall(444, None, 0, 1) > 0 else 1)' 2>/dev/null; then
    LANDLOCK=1
else
    LANDLOCK=0
fi

expect() {   # expect <label> <file> <wanted text>
    grep -qF "$3" "$2" || fail "$1: expected '$3'" "$2"
}

run() {      # run <out> <host args...>
    out="$1"; shift
    "$TMP/host" "$@" > "$out" 2>&1 || fail "host exited non-zero" "$out"
}

G="fs_read=$TMP/*"

if [ "$LANDLOCK" = 1 ]; then
    # ---- Landlock: the raw openat row ----
    run "$TMP/ll_passwd.out" "$G" "$TMP/probe" raw_openat /etc/passwd
    expect "raw openat of an ungranted file" "$TMP/ll_passwd.out" "raw openat /etc/passwd: refused (Permission denied)"
    run "$TMP/ll_fixture.out" "$G" "$TMP/probe" raw_openat "$TMP/fixture.txt"
    expect "raw openat of a granted file" "$TMP/ll_fixture.out" "raw openat $TMP/fixture.txt: ALLOWED"
    echo "  [PASS] sandbox_kernel_layer: Landlock refuses a raw openat the grants do not cover, allows one they do"

    # A name prefix covers the entries that match it, and only those.
    echo x > "$TMP/data-one.txt"; echo y > "$TMP/other.txt"
    run "$TMP/ll_prefix.out" "fs_read=$TMP/data-*,fs_read=$TMP/probe" "$TMP/probe" raw_openat "$TMP/data-one.txt"
    expect "a name-prefix grant" "$TMP/ll_prefix.out" "ALLOWED"
    run "$TMP/ll_prefix2.out" "fs_read=$TMP/data-*,fs_read=$TMP/probe" "$TMP/probe" raw_openat "$TMP/other.txt"
    expect "outside a name-prefix grant" "$TMP/ll_prefix2.out" "refused (Permission denied)"
    echo "  [PASS] sandbox_kernel_layer: a name-prefix grant covers its entries only"

    # A pattern Landlock cannot express is refused, never widened.
    run "$TMP/ll_suffix.out" "fs_read=*.txt" "$TMP/probe" hello
    expect "a suffix grant" "$TMP/ll_suffix.out" "cannot be held by Landlock"
    expect "a suffix grant" "$TMP/ll_suffix.out" "fd=-1 pid=-1"
    echo "  [PASS] sandbox_kernel_layer: a suffix grant is refused with a reason"

    # spawn_sandboxed is under Landlock too.
    "$TMP/sandboxed" "$TMP" "$TMP/probe" raw_passwd > "$TMP/sb.out" 2>&1 || true
    expect "spawn_sandboxed's raw openat" "$TMP/sb.out" "raw openat /etc/passwd: refused (Permission denied)"
    expect "spawn_sandboxed's child" "$TMP/sb.out" "rc=0"
    echo "  [PASS] sandbox_kernel_layer: spawn_sandboxed's child is under Landlock"
else
    echo "  [SKIP] sandbox_kernel_layer: this kernel has no Landlock; only the no-Landlock cases run"
fi

# ---- A kernel with no Landlock ----
"$TMP/nolandlock" "$TMP/host" "$G" "$TMP/probe" raw_openat /etc/passwd > "$TMP/nll.out" 2>&1 || fail "host exited non-zero" "$TMP/nll.out"
expect "spawn_socket with no Landlock" "$TMP/nll.out" "this kernel has no Landlock"
expect "spawn_socket with no Landlock" "$TMP/nll.out" "fd=-1 pid=-1"
echo "  [PASS] sandbox_kernel_layer: no Landlock, spawn_socket refuses"
"$TMP/nolandlock" "$TMP/host" "$G,nolandlock" "$TMP/probe" hello "$TMP/fixture.txt" > "$TMP/nll_opt.out" 2>&1 || fail "host exited non-zero" "$TMP/nll_opt.out"
expect "grant_without_landlock" "$TMP/nll_opt.out" "spawn: ok"
expect "grant_without_landlock" "$TMP/nll_opt.out" "libc fopen /etc/passwd: refused"
echo "  [PASS] sandbox_kernel_layer: grant_without_landlock runs it with the preload"
"$TMP/nolandlock" "$TMP/sandboxed" "$TMP" "$TMP/probe" hello > "$TMP/nll_sb.out" 2>&1 || true
expect "spawn_sandboxed with no Landlock" "$TMP/nll_sb.out" "WARNING: spawn_sandboxed: this kernel has no Landlock"
expect "spawn_sandboxed with no Landlock" "$TMP/nll_sb.out" "rc=0"
echo "  [PASS] sandbox_kernel_layer: no Landlock, spawn_sandboxed still runs its child and says so"
exit 0
