#!/bin/sh
# Per-symbol aliasing in selective imports: `import M (path as vgpath)`
# binds the exported symbol under the alias, frees the original name
# for local use, works for consts and inside module-internal imports,
# and the selective-import shadow guard fires on the ALIAS (the name
# the program actually binds), not the exported name.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] selective_import_alias: ae not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

OUT=$("$AE" run main_alias.ae 2>"$tmpdir/err.log")
EXPECTED="14
path:M0,0
10
path:Z"
if [ "$OUT" != "$EXPECTED" ]; then
    echo "  [FAIL] selective_import_alias: wrong output"
    echo "$OUT" | sed 's/^/    got: /'
    head -5 "$tmpdir/err.log" | sed 's/^/    /'
    exit 1
fi

if "$AE" build main_alias_shadow.ae -o "$tmpdir/shadow" >"$tmpdir/shadow.log" 2>&1; then
    echo "  [FAIL] selective_import_alias: local def named like the alias must be rejected"
    exit 1
fi
if ! grep -q "E1000" "$tmpdir/shadow.log"; then
    echo "  [FAIL] selective_import_alias: expected E1000 shadow diagnostic"
    head -5 "$tmpdir/shadow.log" | sed 's/^/    /'
    exit 1
fi

echo "  [PASS] selective_import_alias: alias binds, original name freed, consts + module-internal + shadow guard"
exit 0
