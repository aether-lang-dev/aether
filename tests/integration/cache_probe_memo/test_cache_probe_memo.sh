#!/bin/sh
# Regression (#3600): the C compiler is asked about its flags once, not by
# every build.
#
# Before each compile, ae asked the C compiler whether it takes
# -fbracket-depth (Clang does, #3574) and, on Windows, whether its assembler
# takes -muse-unaligned-vector-move (#2476), by compiling an empty file with
# each. The answers lived for one process and ae is one process per build,
# so every build paid two extra compiles. They are kept in the cache
# directory now (`<key>.probe`), by the compiler's identity: the first build
# asks and records, later builds read. A record that is gone or unreadable
# is asked again, and the binary is the same either way.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] cache_probe_memo: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR
unset AETHER_HOME
fails=0
fail() { echo "  [FAIL] cache_probe_memo: $1"; fails=$((fails + 1)); }

mkdir -p "$TMP/proj"
cd "$TMP/proj" || exit 1
n=0
# build <what>: a verbose build of a new program (so nothing but the probes'
# answers can come from the cache), run once; the log is "$n.log".
build() {
    n=$((n + 1))
    printf 'main() {\n    println("probe %d")\n}\n' "$n" > "p$n.ae"
    if ! "$AE" build -v "p$n.ae" -o "p$n" > "$n.log" 2>&1; then
        tail -5 "$n.log" | sed 's/^/        /'
        fail "the build $1 failed"
    elif ! "./p$n" | grep -q "^probe $n\$"; then
        fail "the program of the build $1 printed the wrong thing"
    fi
}
# asked: whether the last build ran the -fbracket-depth probe.
asked() { grep -q '^\[cmd\].*ae_bracket_probe' "$n.log"; }

build "with an empty cache"
asked || fail "the first build did not ask about -fbracket-depth"
ls "$AETHER_CACHE_DIR"/*.probe > /dev/null 2>&1 || fail "the first build remembered no answer"

build "after the first"
if grep -E '^\[cmd\].*ae_(bracket|avx)_probe' "$n.log"; then
    fail "a later build asked again"
fi

# A record that is gone is asked again; so is one that cannot be read.
rm -f "$AETHER_CACHE_DIR"/*.probe
build "with the records gone"
asked || fail "with the records gone, the build did not ask"
for f in "$AETHER_CACHE_DIR"/*.probe; do printf 'x' > "$f"; done
build "with the records unreadable"
asked || fail "with the records unreadable, the build did not ask"

# A probe's record is no build: `ae cache` counts the programs alone.
reported=$("$AE" cache 2>/dev/null | sed -n 's/^Cache: \([0-9][0-9]*\) build.*/\1/p' | head -1)
[ "$reported" = "$n" ] || fail "ae cache counts '$reported' builds after $n"

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] cache_probe_memo: the first build asks the compiler about its flags, later builds read the answer"
