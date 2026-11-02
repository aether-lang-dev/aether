#!/bin/sh
# The preload refuses a guest's dlopen of libc or the dynamic loader without
# a native grant, however it is spelt. A handle on libc lets a guest dlsym
# the real open/connect/execve past the preload's own (ctypes.CDLL, Fiddle,
# DynaLoader). The check used to compare the name with "libc.so.6" and
# "libc.so" only, so an absolute path, the /usr/lib twin of /lib, a ".."
# and a symlink with another name all loaded it. It now matches the
# basename of the name and of where the path leads, and then the handle
# itself, so a name the loader finds some other way is caught too.
#
# Landlock (sandbox.spawn_socket) is the wall a raw openat meets on Linux;
# this is the preload layer, tested on its own here.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LIB="$ROOT/build/libaether_sandbox.so"

case "$(uname -s 2>/dev/null)" in
    Linux) ;;
    *)
        echo "  [SKIP] sandbox_dlopen_libc: the probe's spellings are Linux's (this is $(uname -s))"
        exit 0 ;;
esac
if [ ! -f "$LIB" ]; then
    echo "  [SKIP] sandbox_dlopen_libc: preload library not built"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
if ! ${CC:-cc} -std=c11 -Wall -Wextra -Werror \
        "$SCRIPT_DIR/probe.c" -o "$TMP/probe" -ldl; then
    echo "  [FAIL] sandbox_dlopen_libc: probe did not compile"
    exit 1
fi
printf 'env:HOME\n' > "$TMP/none.grants"
printf 'native:*\n' > "$TMP/native.grants"

fail=0

# Outside the sandbox every spelling loads: the probe itself is sound.
LD_LIBRARY_PATH="$TMP" "$TMP/probe" "$TMP" > "$TMP/free.out" 2>&1
if grep -q ' refused$' "$TMP/free.out"; then
    echo "  [FAIL] sandbox_dlopen_libc: without the preload something was refused"
    sed 's/^/        /' "$TMP/free.out"
    exit 1
fi

LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/none.grants" AETHER_SANDBOX_LOG=none LD_LIBRARY_PATH="$TMP" \
    "$TMP/probe" "$TMP" > "$TMP/boxed.out" 2>&1
for label in bare path dotdot symlink search loader other-lib; do
    line=$(grep "^$label " "$TMP/boxed.out")
    if [ -z "$line" ]; then
        case "$label" in
            bare|path|symlink|search)
                echo "  [FAIL] sandbox_dlopen_libc: the probe did not try '$label'"
                fail=1 ;;
            *) echo "  [SKIP] sandbox_dlopen_libc: no '$label' spelling on this system" ;;
        esac
        continue
    fi
    if [ "$line" = "$label refused" ]; then
        echo "  [PASS] dlopen of libc ($label) refused"
    else
        echo "  [FAIL] dlopen of libc ($label) got through: $line"
        fail=1
    fi
done
if grep -qx 'libm loaded' "$TMP/boxed.out"; then
    echo "  [PASS] a library that is not libc still loads"
else
    echo "  [FAIL] libm was refused too"
    fail=1
fi

# A native grant lets them all through, as before.
LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/native.grants" AETHER_SANDBOX_LOG=none LD_LIBRARY_PATH="$TMP" \
    "$TMP/probe" "$TMP" > "$TMP/granted.out" 2>&1
if grep -q ' refused$' "$TMP/granted.out"; then
    echo "  [FAIL] a native:* grant still refused:"
    grep ' refused$' "$TMP/granted.out" | sed 's/^/        /'
    fail=1
else
    echo "  [PASS] a native:* grant loads every spelling"
fi

if [ "$fail" -ne 0 ]; then
    sed 's/^/        boxed: /' "$TMP/boxed.out"
    exit 1
fi
echo "  [PASS] sandbox_dlopen_libc"
