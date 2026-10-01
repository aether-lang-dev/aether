#!/bin/sh
# #1372: scripts/fetch-sqlite-amalgamation.sh fetches the pinned SQLite
# amalgamation and refuses anything else.
#
# Runs against a fixture archive and lock (SQLITE_AMALGAMATION_LOCK, a file://
# URL), so it needs no network and never touches contrib/sqlite/amalgamation.
#
# Asserts:
#   - an archive whose SHA-256 is not the pinned one is refused, and nothing
#     is extracted (no .sha256 record, no sqlite3.c)
#   - the pinned archive extracts exactly sqlite3.c, sqlite3.h, sqlite3ext.h
#     (not shell.c), records its hash, and prints the destination
#   - a second run with a matching record is a no-op that does not download:
#     the lock's URL is pointed at a file that does not exist
#   - an archive missing one of the three files is refused
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FETCH="$ROOT/scripts/fetch-sqlite-amalgamation.sh"

command -v curl >/dev/null 2>&1 || { echo "  [SKIP] sqlite_amalgamation_fetch: needs curl for file:// URLs"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "  [SKIP] sqlite_amalgamation_fetch: needs python3 to build the fixture"; exit 0; }

T="$(mktemp -d)"
trap 'rm -rf "$T" || true' EXIT

fail() {
    echo "  [FAIL] sqlite_amalgamation_fetch: $1"
    [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/    /' | head -10
    exit 1
}

# make_zip OUT FILE... -- an archive shaped like sqlite.org's: one top dir.
make_zip() {
    out="$1"; shift
    python3 - "$out" "$@" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w") as z:
    for name in sys.argv[2:]:
        z.writestr("sqlite-amalgamation-9990000/" + name, "/* fixture " + name + " */\n")
PY
}
sha() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
lock() {  # lock URL SHA
    printf 'SQLITE_VERSION=9.99.0\nSQLITE_URL=%s\nSQLITE_SHA256=%s\nSQLITE_CFLAGS="-DX"\n' "$1" "$2" > "$T/lock"
}
url() {  # file:// URL that a native Windows curl also understands
    p="$1"
    command -v cygpath >/dev/null 2>&1 && p="/$(cygpath -m "$p")"
    printf 'file://%s' "$p"
}
export SQLITE_AMALGAMATION_LOCK="$T/lock"

make_zip "$T/good.zip" sqlite3.c sqlite3.h sqlite3ext.h shell.c
GOOD_SHA="$(sha "$T/good.zip")"

# --- 1. wrong checksum is refused ---------------------------------------
lock "$(url "$T/good.zip")" "0000000000000000000000000000000000000000000000000000000000000000"
if OUT=$(sh "$FETCH" "$T/dest" 2>&1); then fail "a checksum mismatch was accepted" "$OUT"; fi
echo "$OUT" | grep -q "SHA-256 mismatch" || fail "the refusal did not say why" "$OUT"
[ -e "$T/dest/.sha256" ] && fail "a refused archive left a hash record"
[ -e "$T/dest/sqlite3.c" ] && fail "a refused archive was extracted"

# --- 2. the pinned archive extracts the three files ---------------------
lock "$(url "$T/good.zip")" "$GOOD_SHA"
OUT=$(sh "$FETCH" "$T/dest" 2>/dev/null) || fail "the pinned archive was refused" "$(sh "$FETCH" "$T/dest" 2>&1)"
[ "$OUT" = "$T/dest" ] || fail "stdout was not the destination" "$OUT"
for f in sqlite3.c sqlite3.h sqlite3ext.h; do
    [ -f "$T/dest/$f" ] || fail "$f was not extracted"
done
[ -e "$T/dest/shell.c" ] && fail "shell.c was extracted too"
[ "$(cat "$T/dest/.sha256")" = "$GOOD_SHA" ] || fail "the hash record is wrong"

# --- 3. a matching record does not download again ------------------------
lock "$(url "$T/no-such.zip")" "$GOOD_SHA"
OUT=$(sh "$FETCH" "$T/dest" 2>&1) || fail "an already-fetched pin tried to download" "$OUT"
echo "$OUT" | grep -q "downloading" && fail "an already-fetched pin downloaded" "$OUT"

# --- 4. an incomplete archive is refused --------------------------------
make_zip "$T/short.zip" sqlite3.c sqlite3ext.h
lock "$(url "$T/short.zip")" "$(sha "$T/short.zip")"
if OUT=$(sh "$FETCH" "$T/dest2" 2>&1); then fail "an archive without sqlite3.h was accepted" "$OUT"; fi
echo "$OUT" | grep -q "sqlite3.h missing" || fail "the incomplete archive's refusal did not name the file" "$OUT"
[ -e "$T/dest2/.sha256" ] && fail "an incomplete archive left a hash record"

echo "  [PASS] sqlite amalgamation fetch: pinned, verified, idempotent (issue #1372)"
