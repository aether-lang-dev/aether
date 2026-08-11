#!/bin/sh
# Regression (#1494): the runtime must link under EITHER Windows CRT.
#
# A user on msys2 ucrt64 could not compile any program at all:
#
#   libaether.a(aether_locale_num.o): undefined reference to `__imp__snprintf_l'
#   libaether.a(aether_locale_num.o): undefined reference to `__imp__scprintf_l'
#   libaether.a(aether_locale_num.o): undefined reference to `__imp__scprintf'
#
# Note WHERE the bad object was: inside the libaether.a we ship. Our release
# builds it with the MSVCRT-flavoured toolchain, where those symbols resolve.
# The user's linker was ucrt64, which does not export them, so a perfectly
# good archive was unlinkable for half the Windows population. Our own Windows
# CI compiles AND links with msvcrt, so the mismatch could never appear there.
#
# The invariant is therefore not "it compiles on Windows", it is "no object we
# ship depends on which CRT the consumer links". Two checks enforce it:
#
#   1. Source level, no toolchain needed, runs everywhere. The msvcrt-only
#      *_l printf family must not be called at all.
#   2. Link surface, wherever a mingw cross toolchain is installed. Every
#      external symbol the whole MANIFEST needs must be present in BOTH
#      libmsvcrt.a and libucrt.a. This one is exhaustive rather than a list of
#      known-bad names: it asks the CRTs what they actually export.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
rc=0

# --------------------------------------------------------------------------
# 1. Source level: the msvcrt-only *_l printf family.
# --------------------------------------------------------------------------
# The UCRT reimplemented the printf family on top of __stdio_common_vsprintf
# and never exported the _l variants. Their locale-independence has to come
# from a thread-local locale bracket instead (see runtime/aether_locale_num.c).
BANNED='_snprintf_l|_vsnprintf_l|_scprintf_l|_vscprintf_l|_sprintf_l|_vsprintf_l|_printf_l|_fprintf_l|_swprintf_l|_snwprintf_l|_scwprintf_l'

# Deliberately a plain textual match with no attempt to skip comments. A regex
# that tries to tell code from comment is the kind of thing that quietly stops
# matching; refer to them as "the _l-suffixed printf family" in prose instead.
scanned=$(find runtime std -name '*.c' -o -name '*.h' | wc -l | tr -d ' ')
hits=$(grep -rnE "\b($BANNED)\b" runtime std --include='*.c' --include='*.h')
if [ -n "$hits" ]; then
    echo "  [FAIL] windows_crt_symbols: msvcrt-only printf variants the UCRT lacks"
    printf '%s\n' "$hits" | sed 's/^/        /'
    echo "        Use a thread-local locale bracket instead; see"
    echo "        aether_win_snprintf_c_locale in runtime/aether_locale_num.c."
    rc=1
fi

# --------------------------------------------------------------------------
# 2. Link surface: every symbol must exist in both CRTs.
# --------------------------------------------------------------------------
CC_WIN=x86_64-w64-mingw32-gcc
NM_WIN=x86_64-w64-mingw32-nm

if ! command -v "$CC_WIN" >/dev/null 2>&1 || ! command -v "$NM_WIN" >/dev/null 2>&1; then
    if [ "$rc" -eq 0 ]; then
        echo "  [PASS] windows_crt_symbols: $scanned sources free of msvcrt-only printf calls"
        echo "         (link-surface check skipped: no x86_64-w64-mingw32 toolchain)"
    fi
    exit "$rc"
fi

MANIFEST=build/MANIFEST
if [ ! -f "$MANIFEST" ]; then
    echo "  [FAIL] windows_crt_symbols: $MANIFEST missing, run 'make stdlib' first"
    exit 1
fi

LIBUCRT=$("$CC_WIN" -print-file-name=libucrt.a 2>/dev/null)
LIBMSVCRT=$("$CC_WIN" -print-file-name=libmsvcrt.a 2>/dev/null)
if [ ! -f "$LIBUCRT" ] || [ ! -f "$LIBMSVCRT" ]; then
    if [ "$rc" -eq 0 ]; then
        echo "  [PASS] windows_crt_symbols: $scanned sources free of msvcrt-only printf calls"
        echo "         (link-surface check skipped: toolchain ships only one CRT import library)"
    fi
    exit "$rc"
fi

INC="-Iinclude -Iruntime -Iruntime/actors -Iruntime/scheduler -Iruntime/utils
     -Iruntime/memory -Iruntime/config -Istd -Istd/string -Istd/io -Istd/math
     -Istd/net -Istd/collections -Istd/json"

mkdir -p "$TMP/objs"
compiled=0
for src in $(grep -v '^#' "$MANIFEST" | grep -v '^[[:space:]]*$'); do
    [ -f "$src" ] || continue
    obj="$TMP/objs/$(echo "$src" | tr '/' '_' | sed 's/\.c$/.o/')"
    if ! $CC_WIN -O2 $INC -DAETHER_VERSION='"test"' -c "$src" -o "$obj" 2>"$TMP/cc.log"; then
        echo "  [FAIL] windows_crt_symbols: $src does not cross-compile for Windows"
        sed 's/^/        /' "$TMP/cc.log" | head -8
        exit 1
    fi
    compiled=$((compiled + 1))
done

# Undefined symbols across every object, minus the ones we define ourselves.
# The __imp_ prefix is the linker's import-thunk decoration, not part of the
# CRT's exported name, so it comes off before matching.
"$NM_WIN" -u "$TMP"/objs/*.o | awk '{print $2}' | sort -u > "$TMP/undef.txt"
"$NM_WIN" --defined-only "$TMP"/objs/*.o | awk '{print $3}' | sort -u > "$TMP/ours.txt"
comm -23 "$TMP/undef.txt" "$TMP/ours.txt" | sed 's/^__imp_//' | sort -u > "$TMP/external.txt"

crt_exports() {
    "$NM_WIN" --defined-only "$1" 2>/dev/null \
        | awk '$2 ~ /^[TDBRIW]$/ {print $3}' | sed 's/^__imp_//' | sort -u
}
crt_exports "$LIBUCRT"   > "$TMP/ucrt.txt"
crt_exports "$LIBMSVCRT" > "$TMP/msvcrt.txt"

# CRITICAL: a toolchain configured for one CRT often ships the other's import
# library as an alias, and then the two export sets are identical. Comparing a
# set against itself can never fail, so without this guard the check would
# report PASS while proving nothing at all.
if [ "$(comm -3 "$TMP/ucrt.txt" "$TMP/msvcrt.txt" | wc -l | tr -d ' ')" -eq 0 ]; then
    if [ "$rc" -eq 0 ]; then
        echo "  [PASS] windows_crt_symbols: $scanned sources clean; $compiled objects cross-compiled"
        echo "         (CRT comparison skipped: this toolchain's libucrt.a and"
        echo "          libmsvcrt.a export identical sets, so it cannot tell them apart)"
    fi
    exit "$rc"
fi

# A symbol is a hazard when it is in one CRT and not the other. Symbols in
# neither are supplied by libmingwex / libmingw32 / the Win32 DLLs and are CRT
# independent by construction, so they are not this test's business.
only_msvcrt=$(comm -12 "$TMP/external.txt" "$TMP/msvcrt.txt" | comm -23 - "$TMP/ucrt.txt")
only_ucrt=$(comm -12 "$TMP/external.txt" "$TMP/ucrt.txt" | comm -23 - "$TMP/msvcrt.txt")

if [ -n "$only_msvcrt" ]; then
    echo "  [FAIL] windows_crt_symbols: symbols exported by msvcrt but NOT by ucrt"
    printf '%s\n' "$only_msvcrt" | sed 's/^/        /'
    echo "        A ucrt64 toolchain cannot link the archive we ship."
    rc=1
fi
if [ -n "$only_ucrt" ]; then
    echo "  [FAIL] windows_crt_symbols: symbols exported by ucrt but NOT by msvcrt"
    printf '%s\n' "$only_ucrt" | sed 's/^/        /'
    echo "        An msvcrt toolchain cannot link the archive we ship."
    rc=1
fi

if [ "$rc" -eq 0 ]; then
    echo "  [PASS] windows_crt_symbols: $scanned sources clean; $compiled objects, \
$(wc -l < "$TMP/external.txt" | tr -d ' ') external symbols, all CRT-portable"
fi
exit "$rc"
