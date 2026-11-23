#!/bin/sh
# #3836: std.string's length and char_at are `static inline` in
# aether_string_inline.h, which the module asks for with @c_include (the
# #1986 mechanism, no special case in the compiler).
#
#   1. reads.ae's C includes the header and calls the inline reads, not the
#      exported string_length / string_char_at.
#   2. reads.ae checks the semantics the exported forms have: null, an
#      index out of range (negative, past the end, INT_MAX), the byte read
#      unsigned, a length past an embedded NUL, and a scan.
#   3. with_fs.ae imports std.fs too, which binds the same names: both
#      modules' declarations in one program build and agree.
#   4. collide.ae defines its own string_length and imports nothing: the
#      name is still one libaether exports, so codegen renames the
#      program's function (#1366) and the program links.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
[ -f "$AE.exe" ] && AE="$AE.exe"
[ -f "$AETHERC.exe" ] && AETHERC="$AETHERC.exe"
NAME=string_inline_reads

if [ ! -f "$AE" ] || [ ! -f "$AETHERC" ]; then
    echo "  [SKIP] $NAME: ae or aetherc not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AETHER_CACHE_DIR="$TMP/cache"
export AETHER_CACHE_DIR
export AETHER_HOME="$ROOT"
unset AE_CC AE_STRING_INLINE_READS_UNSET
fails=0
fail() { echo "  [FAIL] $NAME: $1"; fails=$((fails + 1)); }

cp "$SCRIPT_DIR"/*.ae "$TMP/"
cd "$TMP" || exit 1

if ! "$AETHERC" reads.ae reads.c > reads.log 2>&1; then
    sed 's/^/        /' reads.log | head -5
    fail "reads.ae did not compile"
else
    grep -q '#include "aether_string_inline.h"' reads.c ||
        fail "reads.ae's C does not include aether_string_inline.h"
    grep -q 'aether_string_length_inline(' reads.c ||
        fail "reads.ae's C does not call aether_string_length_inline"
    grep -q 'aether_string_char_at_inline(' reads.c ||
        fail "reads.ae's C does not call aether_string_char_at_inline"
    # A call, not the prototype every program declares for the `char_at`
    # builtin (`int string_char_at(const char*, int);`).
    calls="$(grep '[^_a-z]string_length(\|[^_a-z]string_char_at(' reads.c |
             grep -v '^ *\(extern \)\{0,1\}int string_[a-z_]*(const char\*' | head -1)"
    [ -z "$calls" ] ||
        fail "reads.ae's C still calls the exported string_length or string_char_at: $(printf '%s' "$calls" | cut -c1-120)"
fi

for opt in "" --quick; do
    if ! "$AE" build $opt reads.ae -o reads > build.log 2>&1; then
        grep -i -m3 "error" build.log | cut -c1-200 | sed 's/^/        /'
        fail "reads.ae did not build ($opt)"
    else
        out="$(./reads 2>&1 | tr -d '\r')"
        [ "$out" = "reads ok" ] || fail "reads.ae ($opt): $out"
    fi
done

if ! "$AE" build with_fs.ae -o with_fs > fs.log 2>&1; then
    grep -i -m3 "error" fs.log | cut -c1-200 | sed 's/^/        /'
    fail "with_fs.ae (std.string and std.fs) did not build"
else
    out="$(./with_fs 2>&1 | tr -d '\r')"
    [ "$out" = "3 121" ] || fail "with_fs.ae printed '$out', not '3 121'"
fi

if ! "$AE" build collide.ae -o collide > collide.log 2>&1; then
    grep -i -m3 "error\|multiple definition" collide.log | cut -c1-200 | sed 's/^/        /'
    fail "collide.ae, whose own function is named string_length, did not build"
else
    out="$(./collide 2>&1 | tr -d '\r')"
    [ "$out" = "7" ] || fail "collide.ae printed '$out', not 7"
fi

if [ "$fails" -ne 0 ]; then
    exit 1
fi
echo "  [PASS] $NAME: length and char_at inline, with the exported forms' semantics"
exit 0
