#!/bin/sh
# Every contrib module with C builds and runs from a binary release, which
# ships contrib's C as source and builds no libaether_<x>.a archives.
#
# 0.801.0's release built `import contrib.sqlite` with
# `ld: library 'aether_sqlite' not found`: the module's @link asks for the
# veneer archive `make contrib` builds, and only a source tree has one.
# contrib.i18n.collate (aether_i18n.c and the DUCET table) and
# contrib.avcodec (aether_avcodec.c) failed at link the same way, their C
# being an --extra the release then trimmed away. Now `ae build` compiles
# contrib.sqlite's veneer and the pinned amalgamation the release ships when
# no archive is there (cached, as the cross build already did), and the
# other two name their C with @source like contrib.jq does.
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

if ! sh "$ROOT/scripts/stage-release.sh" "$ROOT/build" "$rel" --contrib-archives > "$TMP/stage.log" 2>&1; then
    if grep -q -E "fetch-(sqlite|quickjs)" "$TMP/stage.log"; then
        echo "  [SKIP] $NAME: the amalgamations could not be fetched (offline?)"
        exit 0
    fi
    echo "  [FAIL] $NAME: scripts/stage-release.sh failed"
    tail -8 "$TMP/stage.log" | sed 's/^/        /'
    exit 1
fi
# What the archive carries: contrib.sqlite's veneer and the vendored SQLite
# as lib/contrib/ archives (--contrib-archives, what every native release leg
# passes), and nothing else in the way of contrib archives (libaether_main.a
# is the runtime's Windows --emit=obj entry point). aeb's own link names
# `-laether_sqlite -lsqlite3` and never compiles contrib source, so a release
# without them failed `library 'aether_sqlite' not found` for sae and LisMusic.
for a in libaether_sqlite.a libsqlite3.a; do
    [ -f "$rel/lib/contrib/$a" ] || {
        echo "  [FAIL] $NAME: the staged release has no lib/contrib/$a"
        exit 1; }
done
others=$(find "$rel" \( -name 'libaether_*.a' ! -name libaether_main.a ! -name libaether_sqlite.a \
                       -o -name 'libsqlite3.a' -path '*' ! -path "$rel/lib/contrib/*" \) )
if [ -n "$others" ]; then
    echo "  [FAIL] $NAME: unexpected contrib archives in the staged release:"
    echo "$others" | sed 's/^/        /'
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

# The same program with the amalgamation gone from the staged tree: `ae build`
# cannot compile the veneer now, so it links the shipped archives or it fails.
# (A build against the source is the check above; this one is the archives.)
mv "$rel/share/aether/contrib/sqlite/amalgamation" "$TMP/amalgamation.away"
check sqlite_archives 'import contrib.sqlite
main() {
    db, err = sqlite.open(":memory:")
    if err != "" { println("open: ${err}"); return }
    sqlite.exec(db, "create table t(x int); insert into t values (21)")
    rs, qerr = sqlite.query(db, "select x * 2 from t")
    if qerr != "" { println("query: ${qerr}"); return }
    println("sqlite=${sqlite.cell(rs, 0, 0)}")
    sqlite.free(rs)
    sqlite.close(db)
}' "sqlite=42"
mv "$TMP/amalgamation.away" "$rel/share/aether/contrib/sqlite/amalgamation"

# Linked the way aeb links (no `ae build`): the emitted C, libaether.a and the
# module's own @link flags, with only -L lib and -L lib/contrib.
if [ "$EXE" != ".exe" ]; then
    printf '%s\n' 'import contrib.sqlite
main() {
    db, err = sqlite.open(":memory:")
    if err != "" { println("open: ${err}"); return }
    sqlite.close(db)
    println("direct=ok")
}' > "$TMP/direct.ae"
    if ( cd "$TMP" && AETHER_HOME="$rel" "$rel/bin/aetherc" direct.ae direct.c ) > "$TMP/direct.log" 2>&1 \
       && [ -f "$TMP/direct.c" ]; then
        # The module's own link request, as the emitted C's header carries it.
        want=$(sed -n 's|^// aether-link: ||p' "$TMP/direct.c" | head -1)
        cflags=$(AETHER_HOME="$rel" "$rel/bin/ae" cflags 2>/dev/null)
        if ! ${CC:-cc} "$TMP/direct.c" $cflags -L"$rel/lib/contrib" $want \
              -o "$TMP/direct" > "$TMP/direct.cc.log" 2>&1; then
            echo "  [FAIL] $NAME: linking '$want' by hand against the release failed"
            tail -8 "$TMP/direct.cc.log" | sed 's/^/        /'
            fails=$((fails + 1))
        elif [ "$("$TMP/direct" 2>&1)" != "direct=ok" ]; then
            echo "  [FAIL] $NAME: the hand-linked program ran wrong"
            fails=$((fails + 1))
        fi
    else
        echo "  [FAIL] $NAME: aetherc could not compile the direct-link probe"
        tail -8 "$TMP/direct.log" | sed 's/^/        /'
        fails=$((fails + 1))
    fi
fi

check jq 'import contrib.jq
main() {
    out, err = jq.query(".a[1]", "{\"a\": [10, 20, 30]}")
    if err != "" { println("ERR: ${err}") } else { println("jq=${out}") }
}' "jq=20"

check collate 'import contrib.i18n.collate
main() { println("collate=${collate.compare("en", "cafe", "café")}") }' "collate=-1"

# Not on Windows: `ae` links -static there, and MSYS2's static FFmpeg needs
# its whole dependency closure (soxr, the codecs, ...), which only
# `pkg-config --static --libs` knows; that goes in [build] link_flags.
# Windows by the system, not by $EXE: MSYS2's `test -x build/ae` finds
# build/ae.exe, so $EXE is empty there, and this ran (and failed to link)
# wherever MSYS2's FFmpeg is installed.
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) on_windows=1 ;;
    *) on_windows=0 ;;
esac
if [ "$EXE" != ".exe" ] && [ "$on_windows" = 0 ] && command -v pkg-config >/dev/null 2>&1 &&
   pkg-config --exists libavcodec libavformat libavutil libswscale libswresample 2>/dev/null; then
    check avcodec 'import contrib.avcodec
main() {
    dec, err = avcodec.open("no-such-file.mp4", 0, 0)
    if err != "" { println("avcodec=refused") } else { println("avcodec=opened") }
}' "avcodec=refused"
fi

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: contrib.sqlite, jq and i18n.collate build and run from a staged release"
exit 0
