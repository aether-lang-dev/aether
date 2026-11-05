#!/bin/sh
# `ae version` names the release archive this host takes, and `ae version
# install` / `ae upgrade` download that one. ae_version.c used to keep its own
# platform table, which had no FreeBSD row and fell through to linux-x86_64:
# on FreeBSD, `ae version install` fetched and installed the Linux binaries,
# and switching to them left no working ae. The platform now comes from
# tools/ae_release_platform.h; this pins it to what the host says it is.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] ae_version_platform: ae not built"; exit 0; }
[ -x "$AE" ] || AE="$AE.exe"

os="$(uname -s 2>/dev/null)"
arch="$(uname -m 2>/dev/null)"
case "$arch" in
    x86_64|amd64) cpu=x86_64 ;;
    arm64|aarch64) cpu=arm64 ;;
    *) echo "  [SKIP] ae_version_platform: no release target for CPU $arch"; exit 0 ;;
esac
case "$os" in
    Darwin) want="macos-$cpu" ;;
    FreeBSD) want="freebsd-$cpu" ;;
    MINGW*|MSYS*|CYGWIN*) want="windows-$cpu" ;;
    Linux)
        want="linux-$cpu"
        # musl is the non-glibc Linux libc the releases target.
        if ldd --version 2>&1 | grep -qi musl; then want="$want-musl"; fi
        if [ -n "${ANDROID_ROOT:-}" ]; then echo "  [SKIP] ae_version_platform: Android has no release"; exit 0; fi ;;
    *) echo "  [SKIP] ae_version_platform: no release target for $os"; exit 0 ;;
esac

got="$("$AE" version 2>/dev/null | sed -n 's/^Platform: //p' | tr -d '\r' | head -1)"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] ae_version_platform: ae version says platform '$got'; this $os/$arch host takes '$want'"
    exit 1
fi
echo "  [PASS] ae_version_platform: ae version names $want, the archive this host takes"
