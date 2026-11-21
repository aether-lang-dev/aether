#!/bin/sh
# A directory the LD_PRELOAD library refuses reads, to the program listing
# it, the same as one the kernel refuses (chmod 000): opendir, open and
# openat with O_DIRECTORY, scandir, nftw, glob and the opens of an entry
# return what they return for the kernel's EACCES, with errno EACCES set
# last, whatever the library's own log line did on the way.
#
# probe.c prints each call's result and errno; its output under the preload
# (grants that do not reach the directory, the default log) must equal its
# output against a chmod 000 directory, byte for byte. Factor's in-process
# rows hang when the preload refuses its vocabulary roots (aether-guest-
# languages, asks/factor-refused-opendir-hangs-since-0.927.md); this is the
# preload's half of that question.
#
# Lookups (stat, access) are left out: the preload does not hold them (a
# documented difference), where the kernel refuses them inside a chmod 000
# directory.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LIB="$ROOT/build/libaether_sandbox.so"

case "$(uname -s 2>/dev/null)" in
    Linux|FreeBSD) ;;
    *)
        echo "  [SKIP] sandbox_preload_refusal_errno: preload is Linux and FreeBSD only"
        exit 0 ;;
esac
if [ ! -f "$LIB" ]; then
    echo "  [SKIP] sandbox_preload_refusal_errno: preload library not built"
    exit 0
fi
if [ "$(id -u)" = 0 ]; then
    echo "  [SKIP] sandbox_preload_refusal_errno: root reads a chmod 000 directory, so the kernel's refusal cannot be had"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'chmod 755 "$TMP/kernel/d" 2>/dev/null; rm -rf "$TMP"' EXIT
if ! ${CC:-cc} -std=c11 -Wall -Wextra -Werror "$SCRIPT_DIR/probe.c" -o "$TMP/probe"; then
    echo "  [FAIL] sandbox_preload_refusal_errno: probe did not compile"
    exit 1
fi

mkdir -p "$TMP/kernel/d" "$TMP/preload/d" "$TMP/elsewhere"
printf 'x\n' > "$TMP/kernel/d/entry.txt"
printf 'x\n' > "$TMP/preload/d/entry.txt"
chmod 000 "$TMP/kernel/d"
printf 'fs_read:%s/elsewhere/*\n' "$TMP" > "$TMP/grants"
fail=0

(cd "$TMP/kernel" && "$TMP/probe" "$TMP/kernel/d") > "$TMP/kernel.out" 2>&1
# The default log (aether-sandbox.log in the working directory), in a
# directory the grants do not reach.
(cd "$TMP/preload" && LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/grants" \
    "$TMP/probe" "$TMP/preload/d") > "$TMP/preload.out" 2>&1
if ! grep -q 'opendir: NULL errno=EACCES' "$TMP/kernel.out"; then
    echo "  [SKIP] sandbox_preload_refusal_errno: this system does not refuse a chmod 000 directory with EACCES:"
    sed 's/^/    /' "$TMP/kernel.out"
    exit 0
fi
if cmp -s "$TMP/kernel.out" "$TMP/preload.out"; then
    echo "  [PASS] a refused listing reads as the kernel's EACCES, call for call"
else
    echo "  [FAIL] a refused listing differs from the kernel's EACCES (kernel, then preload):"
    diff "$TMP/kernel.out" "$TMP/preload.out" | sed 's/^/    /'
    fail=1
fi
# With the log on stderr, the same, and every refusal logged.
(cd "$TMP/preload" && LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/grants" AETHER_SANDBOX_LOG=stderr \
    "$TMP/probe" "$TMP/preload/d") > "$TMP/stderr.out" 2> "$TMP/stderr.err"
if cmp -s "$TMP/kernel.out" "$TMP/stderr.out" && grep -q "AETHER_DENIED: fs_read $TMP/preload/d" "$TMP/stderr.err"; then
    echo "  [PASS] the same with the log on stderr, the refusal logged"
else
    echo "  [FAIL] with the log on stderr (kernel, then preload):"
    diff "$TMP/kernel.out" "$TMP/stderr.out" | sed 's/^/    /'
    sed 's/^/    /' "$TMP/stderr.err" | head -5
    fail=1
fi

echo ""
echo "sandbox_preload_refusal_errno: $([ "$fail" -eq 0 ] && echo PASS || echo FAIL)"
[ "$fail" -eq 0 ]
