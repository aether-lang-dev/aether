#!/bin/sh
# #2690 follow-up, #2739: the C of programs whose `try` bodies write
# parameters, pattern bindings, catch bindings and struct values compiles
# without a qualifier warning, and so does a try-written optional or sum
# local, now declared volatile (#2902).
#
# A value a `try` body writes is declared volatile, so that it holds after a
# panic what was stored before it. The release and replace of a struct
# that owns strings took a plain pointer, which a volatile struct does not
# convert to, and so did the pointer a store into an element's string field
# takes (`items[i].name = v`): each warned with -Wdiscarded-qualifiers. The
# names a `try` writes were also carried from one receive arm, closure or
# function to the next, so an unrelated local was made volatile and warned
# the same way. A fixed-size array a `try` body writes, passed to a `T[N]`
# parameter, discarded the qualifier of its elements too (#2723).
#
# The regression programs for these cases are compiled to C here, each
# both ways (the GCC path as the C compiler picks it, and the non-GCC one
# forced), at -O0 and -O2, and any qualifier warning fails the test. The
# programs themselves run in the .ae sweep.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
[ -f "$AETHERC.exe" ] && AETHERC="$AETHERC.exe"
CC_BIN="${CC:-cc}"
command -v "$CC_BIN" >/dev/null 2>&1 || CC_BIN=gcc

if [ ! -f "$AETHERC" ]; then
    echo "  [SKIP] try_written_volatile_c: toolchain not built"
    exit 0
fi
if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    echo "  [SKIP] try_written_volatile_c: no C compiler"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"

fail() {
    echo "  [FAIL] try_written_volatile_c: $1"
    exit 1
}

INC="-I$ROOT -I$ROOT/runtime -I$ROOT/runtime/utils -I$ROOT/runtime/actors -I$ROOT/runtime/config -I$ROOT/runtime/scheduler -I$ROOT/runtime/memory -I$ROOT/std -I$ROOT/std/collections -I$ROOT/std/string"

for name in test_try_written_param test_try_written_struct_local test_catch_binding_nested_try \
            test_try_written_array_argument test_issue2902_try_written_optional; do
    src="$ROOT/tests/regression/$name.ae"
    [ -f "$src" ] || fail "$src is missing"
    if ! "$AETHERC" "$src" "$tmp/$name.c" >"$tmp/aetherc.log" 2>&1; then
        sed 's/^/    /' "$tmp/aetherc.log" | head -20
        fail "aetherc failed on $name"
    fi
    { echo "#define AETHER_GCC_COMPAT 0"; cat "$tmp/$name.c"; } > "$tmp/${name}_nogcc.c"
    for c in "$name.c" "${name}_nogcc.c"; do
        for opt in -O0 -O2; do
            # shellcheck disable=SC2086
            if ! $CC_BIN -fsyntax-only $opt $INC "$tmp/$c" >"$tmp/cc.log" 2>&1; then
                sed 's/^/    /' "$tmp/cc.log" | head -20
                fail "$c does not compile at $opt"
            fi
            if grep -qi "discards\|volatile\|qualifier" "$tmp/cc.log"; then
                grep -i "discards\|volatile\|qualifier" "$tmp/cc.log" | sed 's/^/    /' | head -10
                fail "$c at $opt: a volatile value is passed where a plain one is expected"
            fi
        done
    done
done

echo "  [PASS] try_written_volatile_c: try-written parameters, bindings and struct values compile without a qualifier warning"
