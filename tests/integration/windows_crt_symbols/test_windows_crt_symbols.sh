#!/bin/sh
# Regression (#1494): runtime objects must not reference CRT symbols that the
# UCRT does not export.
#
# A user on msys2 ucrt64 could not compile any program at all. Every object
# linked except runtime/aether_locale_num.o, which pulled in msvcrt-era
# `_snprintf_l`, `_scprintf_l` and `_scprintf`:
#
#   undefined reference to `__imp__snprintf_l'
#
# The UCRT is the default CRT on current msys2, and it exports none of the
# *_l printf family. Nothing caught it because the project's own Windows CI
# uses the MSVCRT-flavoured toolchain, where msvcrt.dll does export them.
#
# This compiles the runtime for Windows with a cross toolchain and asserts the
# objects reference no symbol from that banned set. It is a link-surface check,
# not a behaviour test, so it needs no Windows host and no UCRT sysroot.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

CC_WIN=x86_64-w64-mingw32-gcc
NM_WIN=x86_64-w64-mingw32-nm
if ! command -v "$CC_WIN" >/dev/null 2>&1 || ! command -v "$NM_WIN" >/dev/null 2>&1; then
    echo "  [SKIP] windows_crt_symbols: no x86_64-w64-mingw32 toolchain"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Not exported by the UCRT. _scprintf is included because the reporting user's
# ucrt64 could not resolve it either, even though some UCRT builds carry it.
BANNED='_snprintf_l|_scprintf_l|_scprintf|_vsnprintf_l|_swprintf_l'

fail=0
for src in runtime/aether_locale_num.c; do
    obj="$TMP/$(basename "$src" .c).o"
    if ! "$CC_WIN" -O2 -Iinclude -Iruntime -Iruntime/config -Istd \
            -c "$src" -o "$obj" >"$TMP/cc.log" 2>&1; then
        echo "  [FAIL] windows_crt_symbols: $src does not cross-compile for Windows"
        sed 's/^/        /' "$TMP/cc.log" | head -8
        exit 1
    fi
    hits=$("$NM_WIN" -u "$obj" 2>/dev/null | grep -oE "$BANNED" | sort -u)
    if [ -n "$hits" ]; then
        echo "  [FAIL] windows_crt_symbols: $src needs symbols the UCRT lacks"
        printf '%s\n' "$hits" | sed 's/^/        /'
        echo "        These are msvcrt-only. Use setlocale/_configthreadlocale,"
        echo "        which both CRTs export."
        fail=1
    fi
done

[ "$fail" -eq 0 ] || exit 1
echo "  [PASS] windows_crt_symbols: no msvcrt-only CRT symbols in the Windows link surface"
