#!/bin/sh
# scripts/fetch-libxmp.sh fetches the pinned libxmp release and refuses
# anything else.
#
# Runs against a fixture tarball and lock (LIBXMP_LOCK, a file:// URL), so it
# needs no network and never touches contrib/xmp/libxmp.
#
# Asserts:
#   - an archive whose SHA-256 is not the pinned one is refused, and nothing
#     is extracted (no .sha256 record, no sources.txt)
#   - the pinned archive extracts include/, src/ (without src/lite, the
#     libxmp-lite build), COPYING and README, writes sources.txt as exactly
#     Makefile.vc's three object lists (CRLF line ends, backslashed paths),
#     records its hash, and prints the destination
#   - a second run with a matching record is a no-op that does not download
#   - an archive whose Makefile.vc names a source it lacks is refused
#   - a destination that is not an earlier fetch is never replaced
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/portable.sh"
FETCH="$ROOT/scripts/fetch-libxmp.sh"
NAME=libxmp_fetch

command -v curl >/dev/null 2>&1 || { echo "  [SKIP] $NAME: needs curl for file:// URLs"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "  [SKIP] $NAME: needs python3 to build the fixture"; exit 0; }

T="$(mktemp -d)"
trap 'rm -rf "$T" || true' EXIT

fail() {
    echo "  [FAIL] $NAME: $1"
    [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/    /' | head -10
    exit 1
}

# make_tgz OUT N -- a tarball shaped like libxmp's release: one top directory,
# a Makefile.vc listing N sources per object list beyond the files present
# when N > 40 (40 of each list are there; 3 lists, 120 sources).
make_tgz() {
    python3 -I - "$1" "$2" <<'PY'
import io, sys, tarfile
out, listed = sys.argv[1], int(sys.argv[2])
top = "libxmp-9.9.9/"
files = {"include/xmp.h": "/* fixture */\n", "docs/COPYING": "fixture licence\n",
         "README": "fixture\n", "src/lite/lite-player.c": "/* lite */\n",
         "src/loaders/README": "not a source\n"}
lists = {"OBJS": "src\\core%02d", "PROWIZ_OBJS": "src\\loaders\\prowizard\\pw%02d",
         "DEPACKER_OBJS": "src\\depackers\\dp%02d"}
mk = ["CFLAGS = /O2\r\n", "\r\n"]
for var, pat in lists.items():
    mk.append(var + "\t= \\\r\n")
    for i in range(listed):
        name = pat % i
        mk.append(" " + name + ".obj \\\r\n")
        if i < 40:
            files[name.replace("\\", "/") + ".c"] = "/* %s */\n" % name
    mk.append("\r\n")
files["src/player.c"] = "/* player */\n"
mk.append("LITE_OBJS\t= \\\r\n src\\lite\\lite-player.obj \\\r\n\r\n")
files["Makefile.vc"] = "".join(mk)
with tarfile.open(out, "w:gz") as t:
    for n, body in files.items():
        data = body.encode()
        info = tarfile.TarInfo(top + n)
        info.size = len(data)
        t.addfile(info, io.BytesIO(data))
PY
}
sha() { sha256_of "$1"; }
lock() {  # lock URL SHA
    printf 'LIBXMP_VERSION=9.9.9\nLIBXMP_URL=%s\nLIBXMP_SHA256=%s\nLIBXMP_CFLAGS="-DX"\n' "$1" "$2" > "$T/lock"
}
url() {  # file:// URL that a native Windows curl also understands
    p="$1"
    command -v cygpath >/dev/null 2>&1 && p="/$(cygpath -m "$p")"
    printf 'file://%s' "$p"
}
export LIBXMP_LOCK="$T/lock"

make_tgz "$T/good.tar.gz" 40
GOOD_SHA="$(sha "$T/good.tar.gz")"

# --- 1. wrong checksum is refused ---------------------------------------
lock "$(url "$T/good.tar.gz")" "0000000000000000000000000000000000000000000000000000000000000000"
if OUT=$(sh "$FETCH" "$T/dest" 2>&1); then fail "a checksum mismatch was accepted" "$OUT"; fi
echo "$OUT" | grep -q "SHA-256 mismatch" || fail "the refusal did not say why" "$OUT"
[ -e "$T/dest/.sha256" ] && fail "a refused archive left a hash record"
[ -e "$T/dest/sources.txt" ] && fail "a refused archive was extracted"

# --- 2. the pinned archive extracts the full library's sources ----------
lock "$(url "$T/good.tar.gz")" "$GOOD_SHA"
OUT=$(sh "$FETCH" "$T/dest" 2>/dev/null) || fail "the pinned archive was refused" "$(sh "$FETCH" "$T/dest" 2>&1)"
[ "$OUT" = "$T/dest" ] || fail "stdout was not the destination" "$OUT"
for f in include/xmp.h src/player.c src/core00.c src/loaders/prowizard/pw39.c src/depackers/dp07.c COPYING README sources.txt; do
    [ -f "$T/dest/$f" ] || fail "$f was not extracted"
done
[ -e "$T/dest/src/lite" ] && fail "src/lite (libxmp-lite) was extracted too"
n=$(grep -c . "$T/dest/sources.txt")
[ "$n" = 120 ] || fail "sources.txt has $n lines, not Makefile.vc's 120"
grep -q 'lite' "$T/dest/sources.txt" && fail "sources.txt names a libxmp-lite source"
grep -q "$(printf '\r')" "$T/dest/sources.txt" && fail "sources.txt kept Makefile.vc's CR line ends"
[ "$(sed -n 41p "$T/dest/sources.txt")" = "src/loaders/prowizard/pw00.c" ] ||
    fail "sources.txt does not hold Makefile.vc's paths, in order" "$(sed -n 39,42p "$T/dest/sources.txt")"
[ "$(cat "$T/dest/.sha256")" = "$GOOD_SHA" ] || fail "the hash record is wrong"

# --- 3. a matching record does not download again ------------------------
lock "$(url "$T/no-such.tar.gz")" "$GOOD_SHA"
OUT=$(sh "$FETCH" "$T/dest" 2>&1) || fail "an already-fetched pin tried to download" "$OUT"
echo "$OUT" | grep -q "downloading" && fail "an already-fetched pin downloaded" "$OUT"

# --- 4. an archive missing a listed source is refused --------------------
make_tgz "$T/short.tar.gz" 41
lock "$(url "$T/short.tar.gz")" "$(sha "$T/short.tar.gz")"
if OUT=$(sh "$FETCH" "$T/dest2" 2>&1); then fail "an archive lacking a listed source was accepted" "$OUT"; fi
echo "$OUT" | grep -q "core40.c, which the archive lacks" || fail "the refusal did not name the source" "$OUT"
[ -e "$T/dest2/.sha256" ] && fail "an incomplete archive left a hash record"

# --- 5. a directory that is not an earlier fetch is left alone ------------
mkdir -p "$T/mine"
echo keep > "$T/mine/notes.txt"
lock "$(url "$T/good.tar.gz")" "$GOOD_SHA"
if OUT=$(sh "$FETCH" "$T/mine" 2>&1); then fail "a foreign directory was replaced" "$OUT"; fi
[ "$(cat "$T/mine/notes.txt")" = keep ] || fail "a foreign directory's file was touched"

echo "  [PASS] libxmp fetch: pinned, verified, Makefile.vc's source list, idempotent"
