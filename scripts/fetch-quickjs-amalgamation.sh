#!/bin/sh
# Fetch the pinned QuickJS (quickjs-ng) amalgamation for contrib.quickjs.
#
#   scripts/fetch-quickjs-amalgamation.sh [dest-dir]
#
# Reads contrib/quickjs/amalgamation.lock (or $QUICKJS_AMALGAMATION_LOCK),
# downloads QUICKJS_URL, refuses the archive unless its SHA-256 is
# QUICKJS_SHA256, and extracts quickjs-amalgam.c and quickjs.h into dest-dir
# (default contrib/quickjs/amalgamation). Prints dest-dir on success.
#
# Idempotent: dest-dir/.sha256 records the archive the files came from, and a
# matching record with quickjs-amalgam.c present is a no-op with no network
# access. The record is written last, so an interrupted run is a miss, never
# a half-extracted hit. (Modelled on scripts/fetch-sqlite-amalgamation.sh.)
set -eu

root="$(cd "$(dirname "$0")/.." && pwd)"
lock="${QUICKJS_AMALGAMATION_LOCK:-$root/contrib/quickjs/amalgamation.lock}"
dest="${1:-$root/contrib/quickjs/amalgamation}"

[ -f "$lock" ] || { echo "fetch-quickjs: no lock file at $lock" >&2; exit 1; }
QUICKJS_URL=""; QUICKJS_SHA256=""; QUICKJS_VERSION=""
# shellcheck disable=SC1090
. "$lock"
[ -n "$QUICKJS_URL" ] && [ -n "$QUICKJS_SHA256" ] || {
    echo "fetch-quickjs: $lock must set QUICKJS_URL and QUICKJS_SHA256" >&2; exit 1; }

if [ -f "$dest/quickjs-amalgam.c" ] && [ -f "$dest/.sha256" ] &&
   [ "$(cat "$dest/.sha256")" = "$QUICKJS_SHA256" ]; then
    printf '%s\n' "$dest"
    exit 0
fi

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
    else echo "fetch-quickjs: neither sha256sum nor shasum is installed" >&2; return 1
    fi
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
zip="$work/amalgamation.zip"

echo "fetch-quickjs: downloading QuickJS $QUICKJS_VERSION from $QUICKJS_URL" >&2
if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$QUICKJS_URL" -o "$zip" || { echo "fetch-quickjs: download failed" >&2; exit 1; }
elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$zip" "$QUICKJS_URL" || { echo "fetch-quickjs: download failed" >&2; exit 1; }
else
    echo "fetch-quickjs: neither curl nor wget is installed" >&2; exit 1
fi

got="$(sha256_of "$zip")"
if [ "$got" != "$QUICKJS_SHA256" ]; then
    echo "fetch-quickjs: SHA-256 mismatch for $QUICKJS_URL" >&2
    echo "  expected $QUICKJS_SHA256" >&2
    echo "  got      $got" >&2
    exit 1
fi

# The two files sit at the archive's top level. unzip where it exists,
# python3's zipfile where it does not.
out="$work/out"
mkdir -p "$out"
if command -v unzip >/dev/null 2>&1; then
    unzip -q -j -o "$zip" 'quickjs-amalgam.c' 'quickjs.h' -d "$out" 2>/dev/null || true
else
    for py in python3 python; do
        if command -v "$py" >/dev/null 2>&1; then
            "$py" - "$zip" "$out" <<'PY' || true
import os, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
for info in z.infolist():
    name = os.path.basename(info.filename)
    if name in ("quickjs-amalgam.c", "quickjs.h"):
        with z.open(info) as src, open(os.path.join(sys.argv[2], name), "wb") as dst:
            dst.write(src.read())
PY
            break
        fi
    done
fi
for f in quickjs-amalgam.c quickjs.h; do
    [ -f "$out/$f" ] || { echo "fetch-quickjs: $f is not in the archive" >&2; exit 1; }
done

mkdir -p "$dest"
rm -f "$dest/.sha256"
cp "$out/quickjs-amalgam.c" "$out/quickjs.h" "$dest/"
printf '%s\n' "$QUICKJS_SHA256" > "$dest/.sha256"
printf '%s\n' "$dest"
