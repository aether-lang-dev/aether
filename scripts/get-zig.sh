#!/bin/sh
set -eu

# Keep CI and local cross-build reproduction on the same Zig release. Zig 0.16
# is the first release whose WASI libc includes the setjmp headers needed by
# runtime/actors/aether_panic.c.
#
# The version and the archive hashes live in one place, tools/ae_zig_pin.h,
# which `ae toolchain zig` compiles in (#3623). This script reads them from
# there.
#
# When this checkout has a built ae (build/ae), or an ae is on PATH, the work
# is handed to `ae toolchain zig`, which installs into
# <dest>/toolchains/zig-<version>/ on any host it has a pinned archive for.
# Without one, or when it does not produce the pinned zig, the x86_64 Linux
# archive is fetched and checked here, into <dest>/zig-x86_64-linux-<version>/,
# as this script always did. Either way the last line printed is the path of
# a zig that reports the pinned version.
root="$(cd "$(dirname "$0")/.." && pwd)"
pin="$root/tools/ae_zig_pin.h"
ZIG_VERSION="$(sed -n 's/^#define AE_ZIG_VERSION "\(.*\)"$/\1/p' "$pin")"
ZIG_SHA256="$(sed -n 's/^ *{ "x86_64-linux", "\.tar\.xz", "\([0-9a-f]\{64\}\)" },$/\1/p' "$pin")"
if [ -z "$ZIG_VERSION" ] || [ -z "$ZIG_SHA256" ]; then
    echo "get-zig.sh: could not read the Zig pin from $pin" >&2
    exit 1
fi
ZIG_ARCHIVE="zig-x86_64-linux-$ZIG_VERSION.tar.xz"
ZIG_URL="https://ziglang.org/download/$ZIG_VERSION/$ZIG_ARCHIVE"

dest=${1:-.zig}

ae_bin=""
for c in "$root/build/ae" "$root/build/ae.exe"; do
    if [ -x "$c" ]; then ae_bin="$c"; break; fi
done
[ -n "$ae_bin" ] || ae_bin="$(command -v ae 2>/dev/null || true)"
if [ -n "$ae_bin" ]; then
    mkdir -p "$dest"
    if out="$(AETHER_HOME="$dest" "$ae_bin" toolchain zig)"; then
        zig="$(printf '%s\n' "$out" | tail -n 1)"
        if [ -n "$zig" ] && [ "$("$zig" version 2>/dev/null || true)" = "$ZIG_VERSION" ]; then
            printf '%s\n' "$zig"
            exit 0
        fi
    fi
    echo "get-zig.sh: '$ae_bin toolchain zig' did not provide zig $ZIG_VERSION; fetching it here" >&2
fi

zig_dir="$dest/zig-x86_64-linux-$ZIG_VERSION"

if [ ! -x "$zig_dir/zig" ]; then
    mkdir -p "$dest"
    archive="$dest/$ZIG_ARCHIVE"
    curl -fsSL "$ZIG_URL" -o "$archive"
    printf '%s  %s\n' "$ZIG_SHA256" "$archive" | sha256sum -c - >&2
    tar -xJf "$archive" -C "$dest"
fi

test "$("$zig_dir/zig" version)" = "$ZIG_VERSION"
printf '%s\n' "$zig_dir/zig"
