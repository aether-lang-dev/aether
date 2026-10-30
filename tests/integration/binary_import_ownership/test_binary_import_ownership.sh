#!/bin/sh
# Values that cross from a binary library to the program that imports it, and
# to a C host, are released by the runtime that made them (#2931, #2861).
#
#   1. A string an export or a builder returns: the importer copies it into
#      its own runtime and gives the library's back through the release the
#      catalog names. 200 calls a round grow 0 where the heap count is exact,
#      and leaks(1) finds nothing on macOS. Static and shared runtime.
#   2. A C host finds that release in the catalog, reads 1000 results through
#      aether_string_data / aether_string_length and gives each back; leaks(1)
#      finds nothing on macOS.
#   3. A struct holding a string, returned by value, and a builder that frees
#      its config. Neither can be copied between two copies of the runtime, so
#      they need one, the shared runtime: with the static one each library
#      keeps its own copy (on every platform since #3481), and the import is
#      refused, naming the rebuild. On the shared runtime they run and grow 0.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] binary_import_ownership: ae not built"; exit 0; }
CC="${CC:-cc}"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) SO_EXT=".dll";   EXE=".exe" ;;
    Darwin)                          SO_EXT=".dylib"; EXE="" ;;
    *)                               SO_EXT=".so";    EXE="" ;;
esac
LEAKS=0
if [ "$(uname -s)" = Darwin ] && command -v leaks >/dev/null 2>&1; then LEAKS=1; fi

fail() {
    echo "  [FAIL] binary_import_ownership: $1"
    if [ -n "$2" ] && [ -f "$2" ]; then sed 's/^/    /' "$2" | head -30; fi
    exit 1
}

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK" || true' EXIT

# grew_zero <output> <label>: where heap_in_use is exact, a round grew 0.
grew_zero() {
    echo "$1" | grep -q "^exact true grew 0$" && return 0
    echo "$1" | grep -q "^exact false grew" && return 0
    echo "$1"; fail "$2: the heap grew between steady rounds"
}

# no_leaks <label> <command...>: leaks(1) finds nothing at exit (macOS).
no_leaks() {
    label="$1"; shift
    [ "$LEAKS" = 1 ] || return 0
    MallocStackLogging=1 leaks --atExit -- "$@" >leaks.out 2>&1
    grep -q "0 leaks for 0 total leaked bytes" leaks.out || fail "$label: leaks(1) found leaks" leaks.out
}

# build_app <app> <label>: ae build the app and run it; its output in $OUT.
build_app() {
    AETHER_HOME="$ROOT" "$AE" build "$1.ae" -o "$1" >"$1.log" 2>&1 || fail "$2: ae build $1.ae" "$1.log"
    OUT="$("./$1$EXE" 2>&1)" || { echo "$OUT"; fail "$2: $1 failed"; }
}

# refused <app> <what> <label>: importing the library is refused, naming
# what crosses and the rebuild.
refused() {
    if AETHER_HOME="$ROOT" "$AE" build "$1.ae" -o "$1" >"$1.log" 2>&1; then
        fail "$3: importing it was not refused" "$1.log"
    fi
    grep -qF "$2" "$1.log" || fail "$3: the refusal does not say \"$2\"" "$1.log"
    grep -qF "ae build --emit=lib --shared-runtime" "$1.log" || fail "$3: the refusal does not name the rebuild" "$1.log"
}

# duty <library> <names own_runtime: 1 or 0> <label>
duty() {
    AETHER_HOME="$ROOT" "$AE" lib-info "./$1" >"$1.info" 2>&1 || fail "$3: ae lib-info $1" "$1.info"
    has=0; grep -q "Requires:.*own_runtime" "$1.info" && has=1
    [ "$has" = "$2" ] || fail "$3: $1 names own_runtime: $has, expected $2" "$1.info"
}

# run_pass <dir> <library flags> <label> <one runtime: 1 or 0>
run_pass() {
    dir="$1"; flags="$2"; label="$3"; one="$4"
    mkdir -p "$dir"
    cp "$SCRIPT_DIR"/*.ae "$dir/"
    cd "$dir"
    for lib in text rec frees; do
        AETHER_HOME="$ROOT" "$AE" build --emit=lib $flags "$lib.ae" -o "lib$lib$SO_EXT" >"$lib.log" 2>&1 \
            || fail "$label: ae build --emit=lib $lib.ae" "$lib.log"
        rm -f "$lib.ae"
    done
    # #3481: on ELF a library with a runtime of its own names `own_runtime`
    # for what needs one runtime, so an importer of 0.860.0 to 0.864.0, which
    # took that runtime as the program's, refuses it; a library without such
    # a value, one elsewhere, and one on the shared runtime name none.
    want=0; [ "$SO_EXT" = .so ] && [ "$one" = 0 ] && want=1
    duty "libtext$SO_EXT" 0 "$label"
    duty "librec$SO_EXT" "$want" "$label"
    duty "libfrees$SO_EXT" "$want" "$label"

    build_app app_text "$label"
    echo "$OUT" | grep -q "^hi world$" || { echo "$OUT"; fail "$label: text.greet"; }
    echo "$OUT" | grep -q "^intro$" || { echo "$OUT"; fail "$label: text.section"; }
    grew_zero "$OUT" "$label: string results"
    no_leaks "$label: string results" "./app_text$EXE"

    if [ "$one" = 1 ]; then
        build_app app_rec "$label"
        echo "$OUT" | grep -q "^t7 7$" || { echo "$OUT"; fail "$label: rec.tag"; }
        grew_zero "$OUT" "$label: a struct holding a string"
        no_leaks "$label: a struct holding a string" "./app_rec$EXE"
        build_app app_frees "$label"
        echo "$OUT" | grep -q "^kept$" || { echo "$OUT"; fail "$label: frees.keep"; }
    else
        refused app_rec "\`tag\` passes Tag, which holds strings" "$label: a struct holding a string"
        refused app_frees "builder \`keep\` uses its config" "$label: a builder freeing its config"
    fi
    cd "$WORK"
}

run_pass "$WORK/static" "" "static runtime" 0

# The C host, against the static library.
cd "$WORK/static"
LDL=""; [ "$(uname -s)" = Linux ] && LDL="-ldl"
$CC -I"$ROOT/runtime" "$SCRIPT_DIR/host.c" -o "host$EXE" $LDL >host_cc.log 2>&1 || fail "C host: cc" host_cc.log
HOUT="$("./host$EXE" "./libtext$SO_EXT" 2>&1)" || { echo "$HOUT"; fail "C host failed"; }
echo "$HOUT" | grep -q "^host: aether_text_string_release OK$" || { echo "$HOUT"; fail "C host: the catalog's release"; }
no_leaks "C host" "./host$EXE" "./libtext$SO_EXT"
cd "$WORK"

if [ -d "$ROOT/build/shared" ]; then
    run_pass "$WORK/shared" "--shared-runtime" "shared runtime" 1
    echo "  [PASS] binary_import_ownership: returned strings given back (static, shared, C host); one-runtime values refused across two"
else
    echo "  [PASS] binary_import_ownership: returned strings given back (static, C host); one-runtime values refused across two (no shared runtime built here)"
fi
