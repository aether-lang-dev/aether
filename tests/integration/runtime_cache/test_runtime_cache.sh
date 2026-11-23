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
# The whole runtime is compiled in step 1 only, by two first builds at
# once. Every later step is about the cache around it, so it runs against a
# runtime of two small sources of its own (the toolchain's MANIFEST, which
# lists what is compiled, swapped for one that names them): each such build
# compiles two files, archives them and then fails to link a program, which
# needs the real runtime. What those steps check is what ae made and said
# before the link.
#
#   1. Two first programs at once build the runtime archive: both builds
#      succeed and both programs run, and the cache holds one archive and no
#      half-written one. A third program reuses it (`ae build -v` says
#      "(hit)") and runs, and the first again is a cache hit.
#   2. A flag change builds another archive; so does an edit to a runtime
#      source.
#   3. The program's own key: unchanged, it is the same key; with another
#      list of runtime sources (build/MANIFEST), or after an edit to a
#      runtime .c file, it is another. The key hashed the runtime's
#      headers only, so the edit gave the runtime archive a new key while
#      the program was served as it was.
#   4. Under -flto the archive is made by the compiler's LTO-aware archiver
#      (gcc-ar, llvm-ar): plain `ar` gave GCC's slim LTO objects no symbol
#      index, the link failed, and the broken archive was published for
#      every later build. A C program linked against the cached archive
#      with -flto finds its symbols and runs.
#   5. An archive the cache does not take (its slot is a directory) is
#      linked from where it was made and removed after the link: the size
#      cap skips `*.tmp.*`, so it stayed for good.
#   6. The same key fold without --trace, where it rests on a decision the
#      cache remembers: a compiler on another C runtime (a stand-in that
#      answers the C runtime probes with the real compiler and the other
#      runtime's macro, and fails every other compile at once) is refused
#      the archive; the next build reads that decision back without asking,
#      keeps its key, and an edit to a runtime .c file gives it another.

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
# The small runtime of the later steps, in the tree from the start, so the
# tree the whole runtime is keyed by does not change under step 1.
printf 'int rc_tiny_a(int x);\nint rc_tiny_a(int x) { return x + 1; }\n' > "$PRIV/runtime/rc_tiny_a.c"
printf 'int rc_tiny_b(int x);\nint rc_tiny_b(int x) { return x * 2; }\n' > "$PRIV/runtime/rc_tiny_b.c"

mkdir -p "$TMP/proj"
for i in 1 2 3 4 5 6 7 8 9 10 11; do
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
# tiny <n> <log>: the same build against the small runtime; the link fails.
tiny() { (cd "$TMP/proj" && "$AE" build -v --quick --trace "p$1.ae" -o "p$1") > "$TMP/$2" 2>&1; }
# runtime <log>: built, hit or none, from the -v line.
runtime() { tr -d '\r' < "$TMP/$1" | sed -n -E 's/^\[cache\] runtime: .* \((built|hit)\)$/\1/p' | head -1; }
# slot <log>: the archive the -v line names.
slot() { tr -d '\r' < "$TMP/$1" | sed -n -E 's/^\[cache\] runtime: (.*) \((built|hit)\)$/\1/p' | head -1; }
# key <log>: the program's cache key the build publishes under: the one it
# names after aetherc ran, else the one it looked up.
key() {
    k="$(tr -d '\r' < "$TMP/$1" | sed -n 's/^\[cache\] publish key: //p' | head -1)"
    [ -n "$k" ] || k="$(tr -d '\r' < "$TMP/$1" | sed -n -E 's/^\[cache\] (miss|hit): ([0-9a-f]{16})$/\2/p' | head -1)"
    printf '%s' "$k"
}
program_hit() { tr -d '\r' < "$TMP/$1" | grep -q '^\[cache\] hit: '; }
# tmps <dir>: whether a runtime archive's temporary is left in a cache.
tmps() { ls "$1" 2>/dev/null | grep '^rt-.*\.tmp\.' | head -1; }

AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR

# 1. The whole runtime, by two first builds at once. Each runs in a
# subshell, so its own failures are counted here, by its status.
build 1 b1.log & pid1=$!
build 2 b2.log & pid2=$!
wait "$pid1" || fail "one of two concurrent first builds (p1) failed or did not run"
wait "$pid2" || fail "one of two concurrent first builds (p2) failed or did not run"
for l in b1.log b2.log; do
    case "$(runtime $l)" in
        built|hit) ;;
        *) fail "a concurrent first build linked no runtime archive ($l): '$(runtime $l)'" ;;
    esac
done
archives=$(ls "$TMP/cache" 2>/dev/null | grep -c '^rt-.*\.a$')
[ "$archives" = 1 ] || fail "two concurrent first builds left $archives runtime archives, not 1"
[ -z "$(tmps "$TMP/cache")" ] || fail "two concurrent first builds left a half-written runtime archive"
build 3 b3.log && [ "$(runtime b3.log)" != hit ] &&
    fail "a third program did not reuse the runtime archive: '$(runtime b3.log)'"
build 1 b1again.log && ! program_hit b1again.log &&
    fail "a rebuild of an unchanged program was not a cache hit"

# The small runtime from here on.
printf 'runtime/rc_tiny_a.c\nruntime/rc_tiny_b.c\n' > "$PRIV/build/MANIFEST"
AETHER_CACHE_DIR="$TMP/tcache"
export AETHER_CACHE_DIR

# 2 and 3. A flag change, then a source edit.
tiny 3 t3.log
tiny 4 t4.log
tiny 3 t3again.log
a="$(slot t3.log)"
if [ "$(runtime t3.log)" != built ] || [ "$(runtime t4.log)" != hit ] || [ "$(slot t4.log)" != "$a" ]; then
    fail "the small runtime was not built once and reused: '$(runtime t3.log)' then '$(runtime t4.log)'"
fi
[ -n "$(key t3.log)" ] && [ "$(key t3.log)" = "$(key t3again.log)" ] ||
    fail "an unchanged program did not keep its key: '$(key t3.log)', then '$(key t3again.log)'"
# The list of sources is part of what the runtime is: p3 on the whole
# runtime (step 1) and on the small one are two programs.
[ -n "$(key b3.log)" ] && [ "$(key b3.log)" != "$(key t3.log)" ] ||
    fail "a program kept its key when the runtime's source list (build/MANIFEST) changed: $(key t3.log)"
AETHER_CFLAGS_EXTRA="-DAE_RUNTIME_CACHE_FLAG=1"
export AETHER_CFLAGS_EXTRA
tiny 5 t5.log
[ "$(runtime t5.log)" = built ] && [ "$(slot t5.log)" != "$a" ] ||
    fail "a flag change reused the runtime archive: '$(runtime t5.log)' $(slot t5.log)"
unset AETHER_CFLAGS_EXTRA
printf '\n/* runtime_cache: an edit */\n' >> "$PRIV/runtime/rc_tiny_a.c"
tiny 3 t3edit.log
[ "$(runtime t3edit.log)" = built ] && [ "$(slot t3edit.log)" != "$a" ] ||
    fail "an edited runtime source reused the runtime archive: '$(runtime t3edit.log)'"
[ -n "$(key t3edit.log)" ] && [ "$(key t3edit.log)" != "$(key t3.log)" ] ||
    fail "after an edit to a runtime .c file, the program kept the key it was built under before it: $(key t3edit.log)"

# 4. -flto, into a cache of its own: the archive links.
REAL_CC="$(command -v gcc 2>/dev/null || command -v cc 2>/dev/null)"
AETHER_CACHE_DIR="$TMP/lcache"
AETHER_CFLAGS_EXTRA="-flto"
export AETHER_CACHE_DIR AETHER_CFLAGS_EXTRA
tiny 6 t6.log
unset AETHER_CFLAGS_EXTRA
lto="$(slot t6.log)"
if [ "$(runtime t6.log)" != built ] || [ -z "$lto" ]; then
    fail "under -flto no runtime archive was built: $(grep '^\[cache\] runtime' "$TMP/t6.log" | tail -1 | cut -c1-160)"
elif [ -n "$REAL_CC" ]; then
    printf 'int rc_tiny_a(int x);\nint rc_tiny_b(int x);\nint main(void) { return rc_tiny_a(rc_tiny_b(3)) == 7 ? 0 : 1; }\n' > "$TMP/lto_main.c"
    lto_path="$(printf '%s' "$lto" | tr -d '"')"
    if ! "$REAL_CC" -flto "$TMP/lto_main.c" "$lto_path" -o "$TMP/lto_main" > "$TMP/lto_link.log" 2>&1; then
        grep -i "undefined\|error" "$TMP/lto_link.log" | head -2 | cut -c1-200 | sed 's/^/        /'
        fail "a program linked with -flto against the cached runtime archive did not find its symbols"
    elif ! "$TMP/lto_main"; then
        fail "the program linked against the -flto runtime archive did not run"
    fi
fi

# 5. The slot of the edited small runtime's archive (step 2's last) taken
# by a directory, in a cache of its own.
s="$(slot t3edit.log)"
s="${s##*/}"
if [ -z "$s" ]; then
    fail "step 2 left no runtime archive to name the slot of step 5"
else
    AETHER_CACHE_DIR="$TMP/cache3"
    export AETHER_CACHE_DIR
    mkdir -p "$TMP/cache3/$s"
    tiny 9 t9.log
    grep -q "^\[cache\] runtime: .* could not be published; linked from " "$TMP/t9.log" ||
        fail "-v does not say the runtime archive was linked from where it was made: $(grep '^\[cache\] runtime' "$TMP/t9.log" | tail -1 | cut -c1-160)"
    [ -z "$(tmps "$TMP/cache3")" ] ||
        fail "an archive the cache did not take was left in it: $(tmps "$TMP/cache3")"
fi

# 6. Another C runtime, decided once and remembered, then an edit.
crt="$(head -n 1 "$PRIV/build/libaether.crt" 2>/dev/null | tr -d '\r')"
case "$crt" in
    msvcrt) other_def="-D_UCRT" ;;
    glibc)  other_def="-D__UCLIBC__" ;;
    *)      other_def="" ;;
esac
if [ -n "$other_def" ] && [ -n "$REAL_CC" ]; then
    mkdir -p "$TMP/other"
    case "$(uname -s 2>/dev/null)" in
        MINGW*|MSYS*|CYGWIN*)
            printf '@echo off\r\necho %%* | findstr /c:"ae_crt_" >nul\r\nif errorlevel 1 exit /b 1\r\n"%s" %s %%*\r\n' \
                "$(cygpath -w "$REAL_CC")" "$other_def" > "$TMP/other/gcc.cmd"
            OTHER_CC="$(cygpath -m "$TMP/other/gcc.cmd")" ;;
        *)
            printf '#!/bin/sh\ncase "$*" in *ae_crt_*) exec "%s" %s "$@" ;; esac\nexit 1\n' \
                "$REAL_CC" "$other_def" > "$TMP/other/gcc"
            chmod +x "$TMP/other/gcc"
            OTHER_CC="$TMP/other/gcc" ;;
    esac
    AETHER_CACHE_DIR="$TMP/ocache"
    export AETHER_CACHE_DIR
    # other <n> <log>: a build without --trace by that compiler; it fails.
    other() { (cd "$TMP/proj" && CC="$OTHER_CC" "$AE" build -v --quick "p$1.ae" -o "p$1") > "$TMP/$2" 2>&1; }
    other 10 o1.log
    other 10 o2.log
    grep -q "the runtime is compiled from source" "$TMP/o1.log" ||
        fail "the compiler on another C runtime was not refused the archive: $(grep '^\[toolchain\] libaether.a' "$TMP/o1.log" | tail -1 | cut -c1-160)"
    if grep -q "ae_crt_probe" "$TMP/o2.log"; then
        fail "the second build asked again which C runtime the compiler targets: $(grep ae_crt_probe "$TMP/o2.log" | head -1 | cut -c1-160)"
    fi
    [ -n "$(key o1.log)" ] && [ "$(key o1.log)" = "$(key o2.log)" ] ||
        fail "the warm build by a compiler on another C runtime did not keep its key: '$(key o1.log)', then '$(key o2.log)'"
    printf '\n/* runtime_cache: another edit */\n' >> "$PRIV/runtime/rc_tiny_b.c"
    other 10 o3.log
    [ -n "$(key o3.log)" ] && [ "$(key o3.log)" != "$(key o2.log)" ] ||
        fail "after an edit to a runtime .c file, a build on the remembered source fallback kept its key: $(key o3.log)"
else
    echo "  [SKIP] $NAME: step 6: no other C runtime to name for '${crt:-none}'"
fi

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] $NAME: two first builds at once both link and run and leave one runtime archive, reused by the next program; a flag change or a source edit makes another, and the edit gives the program another key, with --trace or on a remembered C runtime refusal; a -flto archive links; an archive the cache does not take is removed after the link"
exit 0
