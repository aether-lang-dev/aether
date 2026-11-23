#!/bin/sh
# Regression (#3806): a local named like a <stdatomic.h> name is renamed in
# the generated C, as other C header names are.
#
# tests/regression/test_local_named_like_stdatomic_in_handler.ae runs; this
# checks its C. A handler local spelled `memory_order_relaxed` reached C
# under that name and shadowed the constant AETHER_STATE_LOAD passes, so the
# state read after it took the user's 99 as its memory order (GCC warned
# "invalid memory model 99" and fell back to seq_cst; nothing failed).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
EXE=""
[ -x "$ROOT/build/aetherc" ] || EXE=".exe"
AETHERC="$ROOT/build/aetherc$EXE"
if [ ! -x "$AETHERC" ]; then
    echo "  [SKIP] stdatomic_local_names: compiler not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
SRC="$ROOT/tests/regression/test_local_named_like_stdatomic_in_handler.ae"
if ! AETHER_HOME="" "$AETHERC" "$SRC" "$TMP/out.c" > "$TMP/gen.log" 2>&1; then
    echo "  [FAIL] stdatomic_local_names: aetherc failed"
    sed 's/^/        /' "$TMP/gen.log" | head -10
    exit 1
fi
fail=0
for name in memory_order_relaxed memory_order_seq_cst atomic_int; do
    if grep -Eq "(^|[^_A-Za-z0-9])$name = " "$TMP/out.c"; then
        echo "  [FAIL] stdatomic_local_names: a local reached C as $name"
        grep -Em2 "(^|[^_A-Za-z0-9])$name = " "$TMP/out.c" | sed 's/^/        /'
        fail=1
    fi
    if ! grep -q "ae_$name = " "$TMP/out.c"; then
        echo "  [FAIL] stdatomic_local_names: no ae_$name in the C"
        fail=1
    fi
done
if [ "$fail" -ne 0 ]; then exit 1; fi
echo "  [PASS] stdatomic_local_names: locals named like <stdatomic.h> names are renamed"
exit 0
