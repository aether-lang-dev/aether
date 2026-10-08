#!/bin/sh
# Regression (#2536): ae reads the generated C's header lines, aether.toml
# and the preprocessor's output a whole line at a time, however long.
#
# 1. A module whose `@link` is a single 3 KB line: 16 long -L directories,
#    then a -D its own C requires (a define rather than a library, so the
#    test needs no archiver: gcc applies it wherever it sits on the command
#    line). Its `@source` file and `@c_include` directory are listed after
#    that line. Read in fixed chunks, the entry-point scan (512 bytes) met
#    a chunk that does not start with `//` and said the program has no
#    main(); the link flags (1 KB) lost the -D; and the @source and
#    @c_include scans (2 KB) stopped at the link line, so the module's C
#    file and include directory never reached the compiler.
# 2. A [[bin]] extra_sources on one line of more than 8 KB, its last entry
#    straddling byte 8191: the line was cut inside that entry, the entry
#    was lost and `", "` was read as a file name.
# 3. `ae bindgen consts` of a macro whose expansion is 2 KB of `1+1+...`:
#    it was cut, and imported as 512 instead of 1000.
#
# And the fixed tables behind those lines (#2537):
#
# 4. A module whose `@link` carries 70 tokens, -D defines its C requires to
#    the last: codegen kept the first 64 and dropped the rest unsaid.
# 5. 260 `@source` files and 65 `@c_include` directories in one import
#    closure, read from the generated C's header: codegen listed 256
#    sources, and 64 headers and directories.
# 6. `ae bindgen consts` of 5000 macros with 63-byte names: the name list
#    stopped at 256 KB and the candidates at 4096, so about 900 were
#    dropped, not even listed as skipped. And a 4 KB string macro whose
#    `-dM` line has `#define ` at byte 4095: read in 4 KB pieces, the piece
#    from there on was taken as a macro of its own.
# 7. A dependency manifest aetherc cannot write is reported (it was said
#    only under --verbose), and the build still runs.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] ae_long_header_lines: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR
fail=0

# --- 1. a 3 KB @link line, then @source and @c_include ---
mkdir -p "$TMP/lib/longhdr" "$TMP/proj"
pad=$(i=0; while [ $i -lt 180 ]; do printf x; i=$((i + 1)); done)
links=""
i=0
while [ $i -lt 16 ]; do
    links="${links}-Lae_no_such_dir_${i}_$pad "
    i=$((i + 1))
done
links="${links}-DAE_LONG_HDR_TAIL=11"
cat > "$TMP/lib/longhdr/module.ae" <<AEOF
@link("$links")
@source("longhdr.c")
@c_include("longhdr.h")
exports(tail, src, inc)
extern lh_tail() -> int
extern lh_src() -> int
extern lh_inc(v: int) -> int @c_import
tail() -> int { return lh_tail() }
src() -> int { return lh_src() }
inc() -> int { return lh_inc(33) }
AEOF
cat > "$TMP/lib/longhdr/longhdr.c" <<'COF'
#ifndef AE_LONG_HDR_TAIL
#error "the last @link flag did not reach the compiler"
#endif
int lh_tail(void) { return AE_LONG_HDR_TAIL; }
int lh_src(void) { return 22; }
COF
cat > "$TMP/lib/longhdr/longhdr.h" <<'HOF'
#ifndef LONGHDR_H
#define LONGHDR_H
static inline int lh_inc(int v) { return v; }
#endif
HOF
cat > "$TMP/proj/main.ae" <<'AEOF'
import longhdr

main() {
    println("tail=${longhdr.tail()} src=${longhdr.src()} inc=${longhdr.inc()}")
}
AEOF
cd "$TMP/proj" || exit 1
if ! "$AE" build --lib "$TMP/lib" main.ae -o "$TMP/longhdr" >"$TMP/build1.log" 2>&1; then
    if grep -q "has no main()" "$TMP/build1.log"; then
        echo "  [FAIL] ae_long_header_lines: the entry point was not found past a 3 KB @link line"
    elif grep -q "last @link flag did not reach" "$TMP/build1.log"; then
        echo "  [FAIL] ae_long_header_lines: the last flag of a 3 KB @link line did not reach the compiler"
    else
        echo "  [FAIL] ae_long_header_lines: the build with a 3 KB @link line failed"
    fi
    sed 's/^/        /' "$TMP/build1.log" | head -10
    fail=1
else
    got=$("$TMP/longhdr" 2>&1 | tr -d '\r')
    if [ "$got" != "tail=11 src=22 inc=33" ]; then
        echo "  [FAIL] ae_long_header_lines: the program printed '$got', expected 'tail=11 src=22 inc=33'"
        fail=1
    else
        echo "  [PASS] ae_long_header_lines: the entry, the last @link flag, @source and @c_include survive a 3 KB @link line"
    fi
fi

# --- 2. a single-line extra_sources over 8 KB ---
# 27 bytes before the padding, 8158 spaces, so the opening quote of the last
# entry is byte 8185 and the old 8192-byte read ended six bytes into it.
mkdir -p "$TMP/toml"
cd "$TMP/toml" || exit 1
cat > main.ae <<'AEOF'
extern es_first() -> int
extern es_last() -> int

main() {
    println("first=${es_first()} last=${es_last()}")
}
AEOF
printf 'int es_first(void) { return 1; }\n' > first.c
printf 'int es_last(void) { return 2; }\n' > last_entry.c
{
    printf '[[bin]]\nname = "main"\npath = "main.ae"\n'
    printf 'extra_sources = ["first.c",'
    i=0
    while [ $i -lt 8158 ]; do printf ' '; i=$((i + 1)); done
    printf '"last_entry.c"]\n'
} > aether.toml
if ! "$AE" build main.ae -o "$TMP/es" >"$TMP/build2.log" 2>&1; then
    echo "  [FAIL] ae_long_header_lines: the build with an 8 KB extra_sources line failed"
    sed 's/^/        /' "$TMP/build2.log" | head -10
    fail=1
else
    got=$("$TMP/es" 2>&1 | tr -d '\r')
    if [ "$got" != "first=1 last=2" ]; then
        echo "  [FAIL] ae_long_header_lines: the program printed '$got', expected 'first=1 last=2'"
        fail=1
    else
        echo "  [PASS] ae_long_header_lines: the last entry of an 8 KB extra_sources line is compiled in"
    fi
fi

# --- 3. ae bindgen consts with a 2 KB expansion ---
cd "$TMP" || exit 1
{
    printf '#define AE_LONG_SUM 1'
    i=1
    while [ $i -lt 1000 ]; do printf '+1'; i=$((i + 1)); done
    printf '\n'
} > "$TMP/sum.h"
if ! "$AE" bindgen consts "$TMP/sum.h" -o "$TMP/sum.ae" >"$TMP/bindgen.log" 2>&1; then
    echo "  [FAIL] ae_long_header_lines: ae bindgen consts failed on a 2 KB expansion"
    sed 's/^/        /' "$TMP/bindgen.log" | head -10
    fail=1
else
    got=$(tr -d '\r' < "$TMP/sum.ae" | grep '^const AE_LONG_SUM = ')
    if [ "$got" != "const AE_LONG_SUM = 1000" ]; then
        echo "  [FAIL] ae_long_header_lines: bindgen wrote '$got', expected 'const AE_LONG_SUM = 1000'"
        fail=1
    else
        echo "  [PASS] ae_long_header_lines: bindgen imports a 2 KB expansion whole"
    fi
fi

# --- 4. an @link of 70 tokens ---
mkdir -p "$TMP/lib/manytok" "$TMP/tok"
defs=""
i=0
while [ $i -lt 70 ]; do defs="${defs}-DAE_LT_$i=1 "; i=$((i + 1)); done
cat > "$TMP/lib/manytok/module.ae" <<AEOF
@link("$defs")
@source("manytok.c")
exports(last)
extern mt_last() -> int
last() -> int { return mt_last() }
AEOF
cat > "$TMP/lib/manytok/manytok.c" <<'COF'
#if !defined(AE_LT_0) || !defined(AE_LT_69)
#error "an @link token past the 64th did not reach the compiler"
#endif
int mt_last(void) { return 70; }
COF
cat > "$TMP/tok/main.ae" <<'AEOF'
import manytok

main() {
    println("last=${manytok.last()}")
}
AEOF
cd "$TMP/tok" || exit 1
if ! "$AE" build --lib "$TMP/lib" main.ae -o "$TMP/manytok" >"$TMP/build4.log" 2>&1; then
    echo "  [FAIL] ae_long_header_lines: the build with a 70-token @link failed"
    sed 's/^/        /' "$TMP/build4.log" | head -10
    fail=1
else
    got=$("$TMP/manytok" 2>&1 | tr -d '\r')
    if [ "$got" != "last=70" ]; then
        echo "  [FAIL] ae_long_header_lines: the program printed '$got', expected 'last=70'"
        fail=1
    else
        echo "  [PASS] ae_long_header_lines: all 70 tokens of an @link reach the compiler"
    fi
fi

# --- 5. 260 @source files and 65 @c_include directories ---
# Read from the header aetherc writes, so no C is compiled.
mkdir -p "$TMP/many/manysrc/s" "$TMP/manyproj"
{
    i=0
    while [ $i -lt 260 ]; do printf '@source("s/f_%d.c")\n' $i; i=$((i + 1)); done
    printf 'exports(zero)\nzero() -> int { return 0 }\n'
} > "$TMP/many/manysrc/module.ae"
i=0
while [ $i -lt 260 ]; do : > "$TMP/many/manysrc/s/f_$i.c"; i=$((i + 1)); done
{
    printf 'import manysrc\n'
    i=0
    while [ $i -lt 65 ]; do printf 'import inc%d\n' $i; i=$((i + 1)); done
    printf '\nmain() {\n    n = manysrc.zero()'
    i=0
    while [ $i -lt 65 ]; do printf ' + inc%d.v()' $i; i=$((i + 1)); done
    printf '\n    println("${n}")\n}\n'
} > "$TMP/manyproj/main.ae"
i=0
while [ $i -lt 65 ]; do
    mkdir -p "$TMP/many/inc$i"
    printf '@c_include("inc%d.h")\nexports(v)\nv() -> int { return %d }\n' $i $i \
        > "$TMP/many/inc$i/module.ae"
    i=$((i + 1))
done
cd "$TMP/manyproj" || exit 1
if ! "$AETHERC" --lib "$TMP/many" main.ae out.c >"$TMP/aetherc5.log" 2>&1; then
    echo "  [FAIL] ae_long_header_lines: aetherc failed on 260 @source and 65 @c_include"
    sed 's/^/        /' "$TMP/aetherc5.log" | head -10
    fail=1
else
    nsrc=$(grep -c '^// aether-source: ' out.c)
    ninc=$(grep -c '^// aether-include: ' out.c)
    nhdr=$(grep -c '^#include "inc[0-9]*\.h"' out.c)
    if [ "$nsrc $ninc $nhdr" != "260 65 65" ]; then
        echo "  [FAIL] ae_long_header_lines: the header lists $nsrc sources, $ninc include directories and $nhdr headers, expected 260, 65 and 65"
        fail=1
    else
        echo "  [PASS] ae_long_header_lines: 260 @source files and 65 @c_include headers and directories reach the header"
    fi
fi

# --- 6. ae bindgen consts: 5000 macros, and a #define inside a 4 KB string ---
cd "$TMP" || exit 1
pad=$(i=0; while [ $i -lt 50 ]; do printf x; i=$((i + 1)); done)
i=0
while [ $i -lt 5000 ]; do
    printf '#define AE_MANY_%s_%04d %d\n' "$pad" $i $i
    i=$((i + 1))
done > "$TMP/many.h"
if ! "$AE" bindgen consts "$TMP/many.h" -o "$TMP/many.ae" >"$TMP/bindgen6.log" 2>&1; then
    echo "  [FAIL] ae_long_header_lines: ae bindgen consts failed on 5000 macros"
    sed 's/^/        /' "$TMP/bindgen6.log" | head -10
    fail=1
elif ! grep -q "5000 consts imported, 0 skipped" "$TMP/bindgen6.log"; then
    echo "  [FAIL] ae_long_header_lines: bindgen did not import all 5000 macros:"
    sed 's/^/        /' "$TMP/bindgen6.log" | head -5
    fail=1
else
    echo "  [PASS] ae_long_header_lines: bindgen imports 5000 macros with 320 KB of names"
fi
# `#define AE_LONG_TEXT "` is 22 bytes, so 4073 more put the `#define`
# inside the string at byte 4095, where the old 4096-byte read was cut.
{
    printf '#define AE_LONG_TEXT "'
    i=0
    while [ $i -lt 4073 ]; do printf x; i=$((i + 1)); done
    printf '#define AE_SPLIT_BOGUS 1"\n'
} > "$TMP/text.h"
if ! "$AE" bindgen consts "$TMP/text.h" -o "$TMP/text.ae" >"$TMP/bindgen7.log" 2>&1; then
    echo "  [FAIL] ae_long_header_lines: ae bindgen consts failed on a 4 KB string macro"
    sed 's/^/        /' "$TMP/bindgen7.log" | head -10
    fail=1
elif ! grep -q "1 consts imported, 0 skipped" "$TMP/bindgen7.log"; then
    echo "  [FAIL] ae_long_header_lines: text inside a 4 KB string was read as a macro:"
    sed 's/^/        /' "$TMP/bindgen7.log" | head -5
    fail=1
else
    echo "  [PASS] ae_long_header_lines: a #define inside a 4 KB string is not a macro"
fi

# --- 7. a dependency manifest that cannot be written ---
# A cold build leaves one manifest in a cache of its own; a directory put
# in its place makes the next one unwritable.
AETHER_CACHE_DIR="$TMP/cache7"
export AETHER_CACHE_DIR
mkdir -p "$TMP/deps"
cd "$TMP/deps" || exit 1
printf 'main() { println("one") }\n' > main.ae
"$AE" run main.ae >/dev/null 2>&1
dep=""
for d in "$AETHER_CACHE_DIR"/*.deps; do [ -f "$d" ] && dep="$d"; done
if [ -z "$dep" ]; then
    echo "  [FAIL] ae_long_header_lines: a cached ae run left no dependency manifest"
    fail=1
else
    rm -f "$dep"
    mkdir "$dep"
    printf 'main() { println("two") }\n' > main.ae
    got=$("$AE" run main.ae 2>"$TMP/run7.err" | tr -d '\r')
    if [ "$got" != "two" ]; then
        echo "  [FAIL] ae_long_header_lines: with the manifest unwritable the program printed '$got', expected 'two'"
        sed 's/^/        /' "$TMP/run7.err" | head -5
        fail=1
    elif ! grep -q "could not write the dependency manifest" "$TMP/run7.err"; then
        echo "  [FAIL] ae_long_header_lines: an unwritable dependency manifest was not reported"
        fail=1
    else
        echo "  [PASS] ae_long_header_lines: an unwritable dependency manifest is reported, and the build runs"
    fi
fi

exit $fail
