#!/bin/sh
# A string stored into a string field of an `extern struct ... @c_import`
# is a plain store. aetherc used to treat the field like one of an
# Aether-defined struct and emit `p->_heap_text = ...` beside it, a field the
# C header does not have, so the generated C did not compile; a struct
# literal of such a type also got ownership trackers, and a local holding
# one was declared with the bare tag (`label v`), which needs a typedef the
# header need not ship. The field borrows the string, as the C API that
# declares it expects.
#
# label.h has no `_heap_text` and no typedef, so the probe builds only when
# every store is plain and every spelling is `struct label`.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

if ! "$AE" build probe.ae -o "$tmpdir/probe" >"$tmpdir/build.log" 2>&1; then
    echo "  [FAIL] c_import_struct_string_field: ae build failed"
    sed 's/^/    /' "$tmpdir/build.log" | head -25
    exit 1
fi

if ! "$tmpdir/probe" > "$tmpdir/run.out" 2>&1; then
    echo "  [FAIL] c_import_struct_string_field: binary failed to run"
    sed 's/^/    /' "$tmpdir/run.out" | head -10
    exit 1
fi

expected="pointer: hello 5
pointer, runtime string: world
C sees: 5
C sees in a literal: 3
literal: world 5
nested: 7 nested
returned: val 42 42
OK"
if [ "$(tr -d '\r' < "$tmpdir/run.out")" != "$expected" ]; then
    echo "  [FAIL] c_import_struct_string_field: unexpected output"
    sed 's/^/    /' "$tmpdir/run.out" | head -10
    exit 1
fi

echo "  [PASS] c_import_struct_string_field"
exit 0
