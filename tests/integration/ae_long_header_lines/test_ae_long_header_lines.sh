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

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

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

exit $fail
