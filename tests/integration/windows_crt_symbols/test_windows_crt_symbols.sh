#!/bin/sh
# Regression (#1494): no object we ship may depend on which Windows CRT the
# consumer links. Enforced at the source level (always) and over the whole
# MANIFEST link surface (wherever a mingw cross toolchain exists).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
rc=0

# 1. Source level: the msvcrt-only *_l printf family.
BANNED='_snprintf_l|_vsnprintf_l|_scprintf_l|_vscprintf_l|_sprintf_l|_vsprintf_l|_printf_l|_fprintf_l|_swprintf_l|_snwprintf_l|_scwprintf_l'

# Plain textual match, comments included: a code-vs-comment regex is the kind
# that quietly stops matching. Say "the _l-suffixed printf family" in prose.
scanned=$(find runtime std -name '*.c' -o -name '*.h' | wc -l | tr -d ' ')
hits=$(grep -rnE "\b($BANNED)\b" runtime std --include='*.c' --include='*.h')
if [ -n "$hits" ]; then
    echo "  [FAIL] windows_crt_symbols: msvcrt-only printf variants the UCRT lacks"
    printf '%s\n' "$hits" | sed 's/^/        /'
    echo "        Use a thread-local locale bracket instead; see"
    echo "        aether_win_snprintf_c_locale in runtime/aether_locale_num.c."
    rc=1
fi

# 2. Link surface: every symbol must exist in both CRTs.
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

# __imp_ is import-thunk decoration, not part of the exported name.
"$NM_WIN" -u "$TMP"/objs/*.o | awk '{print $2}' | sort -u > "$TMP/undef.txt"
"$NM_WIN" --defined-only "$TMP"/objs/*.o | awk '{print $3}' | sort -u > "$TMP/ours.txt"
comm -23 "$TMP/undef.txt" "$TMP/ours.txt" | sed 's/^__imp_//' | sort -u > "$TMP/external.txt"

crt_exports() {
    "$NM_WIN" --defined-only "$1" 2>/dev/null \
        | awk '$2 ~ /^[TDBRIW]$/ {print $3}' | sed 's/^__imp_//' | sort -u
}
crt_exports "$LIBUCRT"   > "$TMP/ucrt.txt"
crt_exports "$LIBMSVCRT" > "$TMP/msvcrt.txt"

# A toolchain configured for one CRT often ships the other's import library as
# an alias. Comparing a set with itself can never fail, so say so instead.
if [ "$(comm -3 "$TMP/ucrt.txt" "$TMP/msvcrt.txt" | wc -l | tr -d ' ')" -eq 0 ]; then
    if [ "$rc" -eq 0 ]; then
        echo "  [PASS] windows_crt_symbols: $scanned sources clean; $compiled objects cross-compiled"
        echo "         (CRT comparison skipped: this toolchain's libucrt.a and"
        echo "          libmsvcrt.a export identical sets, so it cannot tell them apart)"
    fi
    exit "$rc"
fi

# Symbols in neither come from libmingwex / libmingw32 / the Win32 DLLs and are
# CRT independent by construction.
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
