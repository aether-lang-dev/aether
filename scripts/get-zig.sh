#!/bin/sh
set -eu

# Keep CI and local cross-build reproduction on the same Zig release. Zig 0.16
# is the first release whose WASI libc includes the setjmp headers needed by
# runtime/actors/aether_panic.c.
ZIG_VERSION=0.16.0
ZIG_SHA256=70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00
ZIG_ARCHIVE="zig-x86_64-linux-$ZIG_VERSION.tar.xz"
ZIG_URL="https://ziglang.org/download/$ZIG_VERSION/$ZIG_ARCHIVE"

dest=${1:-.zig}
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
