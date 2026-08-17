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

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# The Vulkan headers are header-only and platform-independent, so the host's
# copy is what the cross compile reads. Two things make "just use the host's
# include flags" wrong here:
#
#   - `pkg-config --cflags vulkan` is EMPTY when the headers sit in the default
#     system include directory, which is right for a native compile and useless
#     for a cross one: the MinGW compiler does not search /usr/include.
#   - pointing the cross compiler at /usr/include instead puts glibc on its
#     include path, and it fails in bits/libc-header-start.h.
#
# So the header trees are copied into a scratch directory that holds nothing
# else. vk_video/ is needed as well as vulkan/: vulkan_core.h includes
# vk_video/vulkan_video_codec_h264std.h.
VK_INC_ROOT=""
# pkg-config knows the header directory even when --cflags is empty (which it
# is whenever the headers are in a default system path).
if command -v pkg-config >/dev/null 2>&1; then
    pc_inc="$(pkg-config --variable=includedir vulkan 2>/dev/null)"
    [ -n "$pc_inc" ] && [ -f "$pc_inc/vulkan/vulkan.h" ] && VK_INC_ROOT="$pc_inc"
fi
if [ -z "$VK_INC_ROOT" ]; then
    for d in /opt/homebrew/include /usr/local/include /usr/include; do
        if [ -f "$d/vulkan/vulkan.h" ]; then VK_INC_ROOT="$d"; break; fi
    done
fi
if [ -z "$VK_INC_ROOT" ]; then
    echo "  [SKIP] contrib_vulkan_portability: Vulkan headers not installed"
    exit 0
fi

# -L dereferences: a package manager may expose the include dir as a symlink
# into its own store, and copying the link alone would stage nothing.
mkdir -p "$TMP/inc"
if ! cp -RL "$VK_INC_ROOT/vulkan" "$TMP/inc/" 2>/dev/null; then
    echo "  [SKIP] contrib_vulkan_portability: cannot stage $VK_INC_ROOT/vulkan"
    exit 0
fi
[ -d "$VK_INC_ROOT/vk_video" ] && cp -RL "$VK_INC_ROOT/vk_video" "$TMP/inc/" 2>/dev/null
INC="-I$TMP/inc"

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
