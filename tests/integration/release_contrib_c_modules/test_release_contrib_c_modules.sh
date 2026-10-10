#!/bin/sh
# Every contrib module with C builds and runs from a binary release, which
# ships contrib's C as source and builds no libaether_<x>.a archives.
#
# 0.801.0's release built `import contrib.sqlite` with
# `ld: library 'aether_sqlite' not found`: the module's @link asks for the
# veneer archive `make contrib` builds, and only a source tree has one.
# contrib.tinyweb (ws_handshake.c), contrib.i18n.collate (aether_i18n.c and
# the DUCET table) and contrib.avcodec (aether_avcodec.c) failed at link the
# same way, their C being an --extra the release then trimmed away. Now
# `ae build` compiles contrib.sqlite's veneer and the pinned amalgamation the
# release ships when no archive is there (cached, as the cross build already
# did), and the other three name their C with @source like contrib.jq does.
#
# The release tree is staged by scripts/stage-release.sh, the script every
# release.yml leg packs, so this checks what ships. It fetches the SQLite and
# QuickJS amalgamations (a no-op when they are already in the tree); offline,
# with neither there, the test skips. contrib.avcodec is checked where
# FFmpeg's development libraries are installed, except on Windows (below).
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
NAME=release_contrib_c_modules
EXE="${EXE_EXT:-}"
if [ -z "$EXE" ] && [ ! -x "$ROOT/build/ae" ] && [ -x "$ROOT/build/ae.exe" ]; then
    EXE=".exe"
fi
[ -x "$ROOT/build/ae$EXE" ] || { echo "  [SKIP] $NAME: build/ae not built"; exit 0; }
[ -s "$ROOT/build/MANIFEST" ] || { echo "  [SKIP] $NAME: build/MANIFEST not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT
rel="$TMP/release"

if ! sh "$ROOT/scripts/stage-release.sh" "$ROOT/build" "$rel" > "$TMP/stage.log" 2>&1; then
    if grep -q "fetch-\(sqlite\|quickjs\)" "$TMP/stage.log"; then
        echo "  [SKIP] $NAME: the amalgamations could not be fetched (offline?)"
        exit 0
    fi
    echo "  [FAIL] $NAME: scripts/stage-release.sh failed"
    tail -8 "$TMP/stage.log" | sed 's/^/        /'
    exit 1
fi
# What the archive carries: no contrib archives anywhere in it
# (libaether_main.a is the runtime's Windows --emit=obj entry point).
contrib_archives() {
    find "$rel" -name 'libaether_*.a' ! -name libaether_main.a
    find "$rel" -name 'libsqlite3.a'
}
if contrib_archives | grep -q .; then
    echo "  [FAIL] $NAME: the staged release carries contrib archives:"
    contrib_archives | sed 's/^/        /'
    exit 1
fi

fails=0
# check <label> <program> <expected output>
check() {
    printf '%s\n' "$2" > "$TMP/$1.ae"
    if ! ( cd "$TMP" && AETHER_HOME="$rel" "$rel/bin/ae$EXE" build "$1.ae" -o "$TMP/$1$EXE" ) \
            > "$TMP/$1.log" 2>&1; then
        echo "  [FAIL] $NAME: $1 did not build against the release tree"
        tail -8 "$TMP/$1.log" | sed 's/^/        /'
        fails=$((fails + 1))
        return
    fi
    got="$("$TMP/$1$EXE" 2>&1 | tr -d '\r')"
    if [ "$got" != "$3" ]; then
        echo "  [FAIL] $NAME: $1 ran wrong: expected '$3', got '$got'"
        fails=$((fails + 1))
    fi
}

check sqlite 'import contrib.sqlite
main() {
    db, err = sqlite.open(":memory:")
    if err != "" { println("open: ${err}"); return }
    sqlite.exec(db, "create table t(x int); insert into t values (42)")
    rs, qerr = sqlite.query(db, "select x * 2 from t")
    if qerr != "" { println("query: ${qerr}"); return }
    println("sqlite=${sqlite.cell(rs, 0, 0)}")
    sqlite.free(rs)
    sqlite.close(db)
}' "sqlite=84"

check jq 'import contrib.jq
main() {
    out, err = jq.query(".a[1]", "{\"a\": [10, 20, 30]}")
    if err != "" { println("ERR: ${err}") } else { println("jq=${out}") }
}' "jq=20"

# RFC 6455's own example key and accept value.
check tinyweb 'import contrib.tinyweb
main() { println("accept=${tinyweb.ws_generate_accept_key("dGhlIHNhbXBsZSBub25jZQ==")}") }' \
    "accept=s3pPLMBiTxaQ9kYGzzhZRbK+xOo="

check collate 'import contrib.i18n.collate
main() { println("collate=${collate.compare("en", "cafe", "café")}") }' "collate=-1"

# Not on Windows: `ae` links -static there, and MSYS2's static FFmpeg needs
# its whole dependency closure (soxr, the codecs, ...), which only
# `pkg-config --static --libs` knows; that goes in [build] link_flags.
if [ "$EXE" != ".exe" ] && command -v pkg-config >/dev/null 2>&1 &&
   pkg-config --exists libavcodec libavformat libavutil libswscale libswresample 2>/dev/null; then
    check avcodec 'import contrib.avcodec
main() {
    dec, err = avcodec.open("no-such-file.mp4", 0, 0)
    if err != "" { println("avcodec=refused") } else { println("avcodec=opened") }
}' "avcodec=refused"
fi

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: contrib.sqlite, jq, tinyweb and i18n.collate build and run from a staged release"
exit 0
