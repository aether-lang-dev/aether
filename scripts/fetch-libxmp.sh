#!/bin/sh
# Fetch the pinned libxmp source for contrib.xmp.
#
#   scripts/fetch-libxmp.sh [dest-dir]
#
# Reads contrib/xmp/libxmp.lock (or $LIBXMP_LOCK), downloads LIBXMP_URL,
# refuses the archive unless its SHA-256 is LIBXMP_SHA256, and extracts
# libxmp's include/ and src/ (without src/lite, the libxmp-lite build) plus
# docs/COPYING and README into dest-dir (default contrib/xmp/libxmp). It
# writes dest-dir/sources.txt: the C files libxmp's own Makefile.vc compiles
# for the full library (OBJS, PROWIZ_OBJS, DEPACKER_OBJS), one path per line
# relative to dest-dir. Prints dest-dir on success.
#
# Idempotent: dest-dir/.sha256 records the archive the files came from, and a
# matching record with sources.txt present is a no-op with no network access.
# The record is written last, so an interrupted run is a miss, never a
# half-extracted hit. (Modelled on scripts/fetch-sqlite-amalgamation.sh.)
set -eu

root="$(cd "$(dirname "$0")/.." && pwd)"
lock="${LIBXMP_LOCK:-$root/contrib/xmp/libxmp.lock}"
dest="${1:-$root/contrib/xmp/libxmp}"

[ -f "$lock" ] || { echo "fetch-libxmp: no lock file at $lock" >&2; exit 1; }
LIBXMP_URL=""; LIBXMP_SHA256=""; LIBXMP_VERSION=""
# shellcheck disable=SC1090
. "$lock"
[ -n "$LIBXMP_URL" ] && [ -n "$LIBXMP_SHA256" ] || {
    echo "fetch-libxmp: $lock must set LIBXMP_URL and LIBXMP_SHA256" >&2; exit 1; }

if [ -f "$dest/sources.txt" ] && [ -f "$dest/.sha256" ] &&
   [ "$(cat "$dest/.sha256")" = "$LIBXMP_SHA256" ]; then
    printf '%s\n' "$dest"
    exit 0
fi

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
    elif [ "$(uname -s)" = OpenBSD ]; then sha256 -q "$1"   # base, no sha256sum
    else echo "fetch-libxmp: neither sha256sum nor shasum is installed" >&2; return 1
    fi
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
tgz="$work/libxmp.tar.gz"

echo "fetch-libxmp: downloading libxmp $LIBXMP_VERSION from $LIBXMP_URL" >&2
if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$LIBXMP_URL" -o "$tgz" || { echo "fetch-libxmp: download failed" >&2; exit 1; }
elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$tgz" "$LIBXMP_URL" || { echo "fetch-libxmp: download failed" >&2; exit 1; }
else
    echo "fetch-libxmp: neither curl nor wget is installed" >&2; exit 1
fi

got="$(sha256_of "$tgz")"
if [ "$got" != "$LIBXMP_SHA256" ]; then
    echo "fetch-libxmp: SHA-256 mismatch for $LIBXMP_URL" >&2
    echo "  expected $LIBXMP_SHA256" >&2
    echo "  got      $got" >&2
    exit 1
fi

# The archive holds one top-level directory (libxmp-<version>/). tar where it
# exists, python3's tarfile where it does not.
x="$work/x"
mkdir -p "$x"
if command -v tar >/dev/null 2>&1 && tar -xzf "$tgz" -C "$x" 2>/dev/null; then
    :
elif command -v python3 >/dev/null 2>&1; then
    rm -rf "$x"; mkdir -p "$x"
    python3 -I - "$tgz" "$x" <<'PY'
import sys, tarfile
with tarfile.open(sys.argv[1], "r:gz") as t:
    if hasattr(tarfile, "data_filter"):
        t.extractall(sys.argv[2], filter="data")
    else:
        for m in t.getmembers():
            if m.name.startswith("/") or ".." in m.name.split("/"):
                raise SystemExit("unsafe member " + m.name)
        t.extractall(sys.argv[2])
PY
else
    echo "fetch-libxmp: need tar or python3 to extract the archive" >&2; exit 1
fi
top="$(find "$x" -mindepth 1 -maxdepth 1 -type d | head -1)"
for f in include/xmp.h src/player.c Makefile.vc docs/COPYING README; do
    [ -f "$top/$f" ] || { echo "fetch-libxmp: $f missing from the archive" >&2; exit 1; }
done

# The full library's sources, exactly as Makefile.vc lists them (it uses
# CRLF line ends and backslashed paths): the three object lists, each ended
# by a blank line.
out="$work/out"
mkdir -p "$out"
awk '{ sub(/\r$/, "") }
     /^(OBJS|PROWIZ_OBJS|DEPACKER_OBJS)[ \t]*=/ { on = 1; next }
     on && /^[ \t]*$/ { on = 0 }
     on && /\.obj/ { s = $1; gsub(/\\/, "/", s); sub(/\.obj$/, ".c", s); print s }' \
    "$top/Makefile.vc" > "$out/sources.txt"
n=0
while IFS= read -r s; do
    case "$s" in src/*.c) ;; *) echo "fetch-libxmp: unexpected source '$s' in Makefile.vc" >&2; exit 1 ;; esac
    [ -f "$top/$s" ] || { echo "fetch-libxmp: Makefile.vc names $s, which the archive lacks" >&2; exit 1; }
    n=$((n + 1))
done < "$out/sources.txt"
[ "$n" -gt 100 ] || { echo "fetch-libxmp: only $n sources found in Makefile.vc" >&2; exit 1; }

cp -R "$top/include" "$out/include"
cp -R "$top/src" "$out/src"
rm -rf "$out/src/lite"
cp "$top/docs/COPYING" "$out/COPYING"
cp "$top/README" "$out/README"

# Replace an earlier fetch whole, so no file of another version survives; a
# directory this script did not make is never removed.
if [ -e "$dest" ]; then
    if [ -f "$dest/sources.txt" ] || [ -z "$(ls -A "$dest" 2>/dev/null)" ]; then
        rm -rf "$dest"
    else
        echo "fetch-libxmp: $dest exists and is not an earlier fetch; not replacing it" >&2
        exit 1
    fi
fi
mkdir -p "$(dirname "$dest")"
cp -R "$out" "$dest"
printf '%s' "$LIBXMP_SHA256" > "$dest/.sha256"
echo "fetch-libxmp: libxmp $LIBXMP_VERSION ready in $dest ($n sources)" >&2
printf '%s\n' "$dest"
