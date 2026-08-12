#!/bin/sh
# contrib/vulkan carries a _WIN32 branch (LoadLibraryA instead of dlopen) that
# no CI leg builds: contrib-check runs on Linux only. Without this, an edit to
# that branch would not be noticed until a Windows user hit it.
#
# Cross-compiling it for Windows is enough to catch the realistic failure,
# which is a compile error, not a behaviour difference: the branch is fifteen
# lines of LoadLibraryA/GetProcAddress.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

SRC="contrib/vulkan/aether_vulkan.c"
[ -f "$SRC" ] || { echo "  [SKIP] contrib_vulkan_portability: $SRC missing"; exit 0; }

CC_WIN=x86_64-w64-mingw32-gcc
command -v "$CC_WIN" >/dev/null 2>&1 || {
    echo "  [SKIP] contrib_vulkan_portability: no x86_64-w64-mingw32 toolchain"
    exit 0
}

# The Vulkan headers are header-only and platform-independent, so the host's
# copy is what the cross compile reads.
INC=""
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists vulkan 2>/dev/null; then
    INC="$(pkg-config --cflags vulkan)"
elif [ -f /opt/homebrew/include/vulkan/vulkan.h ]; then
    INC="-I/opt/homebrew/include"
elif [ -f /usr/include/vulkan/vulkan.h ]; then
    INC="-I/usr/include"
else
    echo "  [SKIP] contrib_vulkan_portability: Vulkan headers not installed"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if ! $CC_WIN -std=c99 -O2 -Wall -Wextra -Werror $INC -Icontrib/vulkan \
        -c "$SRC" -o "$TMP/win.o" 2>"$TMP/err"; then
    echo "  [FAIL] contrib_vulkan_portability: the Windows branch does not compile"
    sed 's/^/        /' "$TMP/err" | head -10
    exit 1
fi

# It must reach the Win32 loader API, not dlopen: a #ifdef that silently took
# the POSIX arm would compile and then fail to find a loader on Windows.
if ! x86_64-w64-mingw32-nm -u "$TMP/win.o" | grep -q 'LoadLibraryA'; then
    echo "  [FAIL] contrib_vulkan_portability: the object does not reference LoadLibraryA"
    echo "        the _WIN32 arm was compiled out"
    exit 1
fi
if x86_64-w64-mingw32-nm -u "$TMP/win.o" | grep -q '\bdlopen\b'; then
    echo "  [FAIL] contrib_vulkan_portability: the Windows object references dlopen"
    exit 1
fi

echo "  [PASS] contrib_vulkan_portability: the Windows branch compiles and uses LoadLibraryA"
