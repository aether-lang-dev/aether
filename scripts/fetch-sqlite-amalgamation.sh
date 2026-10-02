#!/bin/sh
# Fetch the pinned SQLite amalgamation for contrib.sqlite (#1372).
#
#   scripts/fetch-sqlite-amalgamation.sh [dest-dir]
#
# Reads contrib/sqlite/amalgamation.lock (or $SQLITE_AMALGAMATION_LOCK),
# downloads SQLITE_URL, refuses the archive unless its SHA-256 is
# SQLITE_SHA256, and extracts sqlite3.c, sqlite3.h and sqlite3ext.h into
# dest-dir (default contrib/sqlite/amalgamation). Prints dest-dir on success.
#
# Idempotent: dest-dir/.sha256 records the archive the files came from, and a
# matching record with sqlite3.c present is a no-op with no network access.
# The record is written last, so an interrupted run is a miss, never a
# half-extracted hit.
set -eu

root="$(cd "$(dirname "$0")/.." && pwd)"
lock="${SQLITE_AMALGAMATION_LOCK:-$root/contrib/sqlite/amalgamation.lock}"
dest="${1:-$root/contrib/sqlite/amalgamation}"

[ -f "$lock" ] || { echo "fetch-sqlite: no lock file at $lock" >&2; exit 1; }
SQLITE_URL=""; SQLITE_SHA256=""; SQLITE_VERSION=""
# shellcheck disable=SC1090
. "$lock"
[ -n "$SQLITE_URL" ] && [ -n "$SQLITE_SHA256" ] || {
    echo "fetch-sqlite: $lock must set SQLITE_URL and SQLITE_SHA256" >&2; exit 1; }

if [ -f "$dest/sqlite3.c" ] && [ -f "$dest/.sha256" ] &&
   [ "$(cat "$dest/.sha256")" = "$SQLITE_SHA256" ]; then
    printf '%s\n' "$dest"
    exit 0
fi

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
    else echo "fetch-sqlite: neither sha256sum nor shasum is installed" >&2; return 1
    fi
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
zip="$work/amalgamation.zip"

echo "fetch-sqlite: downloading SQLite $SQLITE_VERSION from $SQLITE_URL" >&2
if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$SQLITE_URL" -o "$zip" || { echo "fetch-sqlite: download failed" >&2; exit 1; }
elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$zip" "$SQLITE_URL" || { echo "fetch-sqlite: download failed" >&2; exit 1; }
else
    echo "fetch-sqlite: neither curl nor wget is installed" >&2; exit 1
fi

got="$(sha256_of "$zip")"
if [ "$got" != "$SQLITE_SHA256" ]; then
    echo "fetch-sqlite: SHA-256 mismatch for $SQLITE_URL" >&2
    echo "  expected $SQLITE_SHA256" >&2
    echo "  got      $got" >&2
    exit 1
fi

# The archive holds one top-level directory; take the three files by
# basename. unzip where it exists, python3's zipfile where it does not
# (minimal containers and some Windows shells ship neither unzip nor busybox).
out="$work/out"
mkdir -p "$out"
if command -v unzip >/dev/null 2>&1; then
    # Not fatal here: an archive lacking one of the files is reported below
    # by name, rather than as unzip's "filename not matched".
    unzip -q -j -o "$zip" '*/sqlite3.c' '*/sqlite3.h' '*/sqlite3ext.h' -d "$out" 2>/dev/null || true
elif command -v python3 >/dev/null 2>&1; then
    python3 - "$zip" "$out" <<'PY'
import os, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
for n in z.namelist():
    if os.path.basename(n) in ("sqlite3.c", "sqlite3.h", "sqlite3ext.h"):
        with open(os.path.join(sys.argv[2], os.path.basename(n)), "wb") as f:
            f.write(z.read(n))
PY
else
    echo "fetch-sqlite: need unzip or python3 to extract the archive" >&2; exit 1
fi
for f in sqlite3.c sqlite3.h sqlite3ext.h; do
    [ -f "$out/$f" ] || { echo "fetch-sqlite: $f missing from the archive" >&2; exit 1; }
done

mkdir -p "$dest"
rm -f "$dest/.sha256"
for f in sqlite3.c sqlite3.h sqlite3ext.h; do
    cp "$out/$f" "$dest/$f"
done
printf '%s' "$SQLITE_SHA256" > "$dest/.sha256"
echo "fetch-sqlite: SQLite $SQLITE_VERSION ready in $dest" >&2
printf '%s\n' "$dest"
