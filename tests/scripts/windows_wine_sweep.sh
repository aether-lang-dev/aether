#!/usr/bin/env bash
#
# Windows RUNTIME coverage from a Linux runner (#1876).
#
# The split that makes this work, and the one #1593 got wrong:
#
#   .ae -> C -> .exe    on the LINUX HOST, native `ae` + `zig cc -target
#                       x86_64-windows-gnu`.  No toolchain inside Wine.
#   run the .exe        under Wine.  Wine never compiles anything.
#
# #1593 ran `ae` itself inside Wine. `ae` is a compile-and-run driver, so it
# went looking for a PE gcc.exe in the prefix and tried to download 250 MB of
# MinGW-w64 mid-job. That is the blocker this shape removes: Wine only ever
# executes finished binaries here.
#
# What this catches that nothing else does: Windows behaviour that COMPILES.
# The existing fast lane proves the toolchain cross-builds; the MSYS2 lane
# runs things but arrives ~25 minutes in. A test that builds for Windows and
# then behaves differently there had no earlier signal at all.
#
# Exclusions live in tests/windows_wine_exclude.txt, one path per line, each
# with a reason. A test that fails because WINDOWS differs is a finding, not
# an exclusion.
#
# Usage:  bash tests/scripts/windows_wine_sweep.sh [dir ...]
# Env:    WINE=wine    ZIG=zig    JOBS=$(nproc)

set -u
cd "$(dirname "$0")/../.." || exit 1
ROOT="$(pwd)"
AE="$ROOT/build/ae"
WINE="${WINE:-wine}"
JOBS="${JOBS:-$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) )}"
EXCLUDE_FILE="$ROOT/tests/windows_wine_exclude.txt"

DIRS=("$@")
if [ "${#DIRS[@]}" -eq 0 ]; then
    DIRS=(tests/regression tests/compiler tests/syntax)
fi

if [ ! -x "$AE" ]; then
    echo "SKIP: build/ae not found; run 'make ae stdlib' first."
    exit 0
fi
if ! command -v "$WINE" > /dev/null 2>&1; then
    echo "SKIP: $WINE not on PATH; this sweep needs Wine to execute the PE binaries."
    exit 0
fi

# A cross build needs zig; without it every single file would BUILDFAIL and the
# sweep would report a wall of red that says nothing about the code.
if ! command -v "${ZIG:-zig}" > /dev/null 2>&1; then
    echo "SKIP: zig not on PATH; ae build --target=x86_64-windows needs it."
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# Wine writes to its prefix on first use; do that once, serially, rather than
# letting N parallel jobs race to create it.
export WINEPREFIX="${WINEPREFIX:-$tmpdir/wineprefix}"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree=d;mshtml=d}"
"$WINE" wineboot --init > "$tmpdir/wineboot.log" 2>&1 || true

# Build the candidate list, minus the exclusions.
: > "$tmpdir/all.txt"
for d in "${DIRS[@]}"; do
    [ -d "$d" ] && find "$d" -name '*.ae' -print >> "$tmpdir/all.txt"
done
sort -o "$tmpdir/all.txt" "$tmpdir/all.txt"

if [ -f "$EXCLUDE_FILE" ]; then
    sed 's/#.*//; s/[[:space:]]*$//; /^$/d' "$EXCLUDE_FILE" | sort -u > "$tmpdir/exclude.txt"
else
    : > "$tmpdir/exclude.txt"
fi
comm -23 "$tmpdir/all.txt" "$tmpdir/exclude.txt" > "$tmpdir/todo.txt"

total=$(wc -l < "$tmpdir/todo.txt" | tr -d ' ')
skipped=$(wc -l < "$tmpdir/exclude.txt" | tr -d ' ')
echo "Windows/Wine runtime sweep: $total files, $skipped excluded, $JOBS jobs"

# One file: cross-build on the host, then execute under Wine.
runner="$tmpdir/one.sh"
cat > "$runner" <<'ONE'
#!/bin/sh
f="$1"; tmpdir="$2"; root="$3"; ae="$4"; wine="$5"
name=$(printf '%s' "$f" | tr '/.' '__')
exe="$tmpdir/$name.exe"

if ! "$ae" build --target=x86_64-windows "$f" -o "$exe" > "$tmpdir/$name.build" 2>&1; then
    printf 'BUILDFAIL %s\n' "$f" >> "$tmpdir/results"
    exit 0
fi
# A cross build that quietly produced a host binary would make the run half
# meaningless, so require a PE before trusting the result.
if command -v file > /dev/null 2>&1; then
    if ! file "$exe" | grep -q 'PE32+'; then
        printf 'NOTPE %s\n' "$f" >> "$tmpdir/results"
        exit 0
    fi
fi
if "$wine" "$exe" > "$tmpdir/$name.run" 2>&1; then
    printf 'PASS %s\n' "$f" >> "$tmpdir/results"
else
    printf 'RUNFAIL %s\n' "$f" >> "$tmpdir/results"
fi
ONE
chmod +x "$runner"

: > "$tmpdir/results"
xargs -a "$tmpdir/todo.txt" -P "$JOBS" -I{} "$runner" "{}" "$tmpdir" "$ROOT" "$AE" "$WINE"

pass=$(grep -c '^PASS '      "$tmpdir/results" 2>/dev/null || echo 0)
runf=$(grep -c '^RUNFAIL '   "$tmpdir/results" 2>/dev/null || echo 0)
bldf=$(grep -c '^BUILDFAIL ' "$tmpdir/results" 2>/dev/null || echo 0)
notpe=$(grep -c '^NOTPE '    "$tmpdir/results" 2>/dev/null || echo 0)

echo ""
echo "  PASS      $pass"
echo "  RUNFAIL   $runf"
echo "  BUILDFAIL $bldf"
echo "  NOT-PE    $notpe"

if [ "$runf" -gt 0 ] || [ "$bldf" -gt 0 ] || [ "$notpe" -gt 0 ]; then
    echo ""
    echo "=== failures ==="
    grep -vE '^PASS ' "$tmpdir/results" | sort | while read -r status f; do
        printf '  %-10s %s\n' "$status" "$f"
        name=$(printf '%s' "$f" | tr '/.' '__')
        case "$status" in
            BUILDFAIL) tail -4 "$tmpdir/$name.build" 2>/dev/null | sed 's/^/             /' ;;
            RUNFAIL)   tail -4 "$tmpdir/$name.run"   2>/dev/null | sed 's/^/             /' ;;
        esac
    done
    exit 1
fi

echo ""
echo "All $pass files built for Windows and ran under Wine."
