#!/bin/sh
# #3813: a build that cannot link libaether.a (a compiler on another C
# runtime, or --trace, #1333) compiles the runtime from source. It did that
# for every new program, 10 to 47 s each. The runtime is compiled once per
# compiler, flags and sources now, kept as an archive in the build cache,
# and linked by every later program.
#
# --trace takes that path with any compiler, so this test needs no other
# one. It builds against a toolchain of its own (copies of the tree's build
# products, its sources linked, runtime/ copied so it can be edited), so an
# edit stays here.
#
#   1. A first program builds the runtime archive; a second, different
#      program reuses it (`ae build -v` says "(built)", then "(hit)"), and
#      both run.
#   2. A flag change builds another; so does an edit to a runtime source.
#   3. Two first builds at once both succeed and run, and the cache holds
#      one archive and no half-written one.
#   4. The program's own key: a rebuild of an unchanged program is a cache
#      hit, and after an edit to a runtime .c file it is not. The key hashed
#      the runtime's headers only, so the edit gave the runtime archive a new
#      key while the program was served as it was.
#   5. Under -flto the archive is made by the compiler's LTO-aware archiver
#      (gcc-ar, llvm-ar): plain `ar` gave GCC's slim LTO objects no symbol
#      index, the link failed, and the broken archive was published for
#      every later build. Two programs build, the second from the cache.
#   6. An archive the cache does not take (its slot is a directory) is
#      linked from where it was made and removed after the link: the size
#      cap skips `*.tmp.*`, so it stayed for good.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
NAME=runtime_cache

if [ ! -x "$ROOT/build/ae" ] && [ ! -x "$ROOT/build/ae.exe" ]; then
    echo "  [SKIP] $NAME: ae not built"
    exit 0
fi
if [ ! -s "$ROOT/build/MANIFEST" ]; then
    echo "  [SKIP] $NAME: no build/MANIFEST (make stdlib)"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
unset AETHER_HOME AE_CC CC AETHER_CFLAGS_EXTRA
fails=0
fail() { echo "  [FAIL] $NAME: $1"; fails=$((fails + 1)); }

# The private toolchain: runtime/ copied, std/ and include/ linked, and the
# build products ae finds beside itself.
PRIV="$TMP/tc"
mkdir -p "$PRIV/build"
cp -R "$ROOT/runtime" "$PRIV/runtime"
ln -s "$ROOT/std" "$PRIV/std"
ln -s "$ROOT/include" "$PRIV/include"
for f in ae ae.exe aetherc aetherc.exe libaether.a libaether_main.a libaether.crt MANIFEST aether_manifest.o; do
    [ -f "$ROOT/build/$f" ] && cp "$ROOT/build/$f" "$PRIV/build/"
done
AE="$PRIV/build/ae"

mkdir -p "$TMP/proj"
for i in 1 2 3 4 5 6 7 8 9; do
    printf 'main() {\n    n = %d\n    println("program ${n}")\n}\n' "$i" > "$TMP/proj/p$i.ae"
done

# build <n> <log>: a --trace build of p<n>, run; says so when either fails.
build() {
    if ! (cd "$TMP/proj" && "$AE" build -v --quick --trace "p$1.ae" -o "p$1") > "$TMP/$2" 2>&1; then
        grep -i "error" "$TMP/$2" | head -3 | cut -c1-200 | sed 's/^/        /'
        fail "the --trace build of p$1 failed"
        return 1
    fi
    got="$("$TMP/proj/p$1" | tr -d '\r')"
    [ "$got" = "program $1" ] || { fail "p$1 printed '$got'"; return 1; }
    return 0
}
# runtime <log>: built, hit or none, from the -v line.
runtime() { tr -d '\r' < "$TMP/$1" | sed -n -E 's/^\[cache\] runtime: .* \((built|hit)\)$/\1/p' | head -1; }
# program_hit <log>: whether the program itself came from the cache.
program_hit() { tr -d '\r' < "$TMP/$1" | grep -q '^\[cache\] hit: '; }

AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR

# 1.
build 1 b1.log && [ "$(runtime b1.log)" != built ] &&
    fail "the first program did not build the runtime archive: '$(runtime b1.log)'"
build 2 b2.log && [ "$(runtime b2.log)" != hit ] &&
    fail "a second program did not reuse the runtime archive: '$(runtime b2.log)'"

# 2. A flag change, then a source edit.
AETHER_CFLAGS_EXTRA="-DAE_RUNTIME_CACHE_FLAG=1"
export AETHER_CFLAGS_EXTRA
build 3 b3.log && [ "$(runtime b3.log)" != built ] &&
    fail "a flag change reused the runtime archive: '$(runtime b3.log)'"
unset AETHER_CFLAGS_EXTRA
# 4, before the edit: the unchanged p1 again is a hit.
build 1 b1again.log && ! program_hit b1again.log &&
    fail "a rebuild of an unchanged program was not a cache hit"
printf '\n/* runtime_cache: an edit */\n' >> "$PRIV/runtime/aether_runtime.c"
build 4 b4.log && [ "$(runtime b4.log)" != built ] &&
    fail "an edited runtime source reused the runtime archive: '$(runtime b4.log)'"
# 4, after it: p1 is built again, against the edited runtime.
build 1 b1edit.log && program_hit b1edit.log &&
    fail "after an edit to a runtime .c file, the program built before it was served from the cache"

# 3. Two first builds at once, into a cache of their own.
AETHER_CACHE_DIR="$TMP/cache2"
export AETHER_CACHE_DIR
build 5 b5.log & pid5=$!
build 6 b6.log & pid6=$!
wait "$pid5" || fail "one of two concurrent first builds failed"
wait "$pid6" || fail "one of two concurrent first builds failed"
archives=$(ls "$TMP/cache2" 2>/dev/null | grep -c '^rt-.*\.a$')
[ "$archives" = 1 ] || fail "two concurrent first builds left $archives runtime archives, not 1"
if ls "$TMP/cache2" 2>/dev/null | grep -q '^rt-.*\.tmp\.'; then
    fail "two concurrent first builds left a half-written runtime archive"
fi

# 5. -flto, into a cache of its own.
AETHER_CACHE_DIR="$TMP/cache_lto"
AETHER_CFLAGS_EXTRA="-flto"
export AETHER_CACHE_DIR AETHER_CFLAGS_EXTRA
build 7 b7.log && [ "$(runtime b7.log)" != built ] &&
    fail "under -flto the first program did not build the runtime archive: '$(runtime b7.log)'"
build 8 b8.log && [ "$(runtime b8.log)" != hit ] &&
    fail "under -flto a second program did not reuse the runtime archive: '$(runtime b8.log)'"
unset AETHER_CFLAGS_EXTRA

# 6. The slot of this tree's archive (the one step 3 made) taken by a
# directory, in a cache of its own.
slot="$(ls "$TMP/cache2" 2>/dev/null | grep '^rt-.*\.a$' | head -1)"
if [ -z "$slot" ]; then
    fail "step 3 left no runtime archive to name the slot of step 6"
else
    AETHER_CACHE_DIR="$TMP/cache3"
    export AETHER_CACHE_DIR
    mkdir -p "$TMP/cache3/$slot"
    build 9 b9.log
    if ls "$TMP/cache3" 2>/dev/null | grep -q '^rt-.*\.tmp\.'; then
        fail "an archive the cache did not take was left in it: $(ls "$TMP/cache3" | grep '\.tmp\.' | head -1)"
    fi
fi

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: the runtime is compiled once and reused by the next program; a flag change or a source edit rebuilds it, and the edit rebuilds the program too; two first builds at once both link; -flto archives link and are reused; an archive the cache does not take is removed after the link"
exit 0
