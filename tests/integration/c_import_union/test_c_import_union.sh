#!/bin/sh
# `extern union Name @c_import` (#2561): a union a C header defines, spelled
# `union Name` in the generated C wherever a tag is needed (sizeof, a pointer
# cast, a parameter or return type). It used to be declared `extern struct`,
# the only form there was, and spelled `struct Name`, a tag mismatch C
# compilers reject ("use of 'X' with tag type that does not match previous
# declaration"). onion.h defines the union without a typedef, so only the
# `union` spelling builds.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
EXE="${EXE_EXT:-}"
if [ -z "$EXE" ] && [ ! -x "$ROOT/build/ae" ] && [ -x "$ROOT/build/ae.exe" ]; then
    EXE=".exe"
fi
AE="$ROOT/build/ae$EXE"
if [ ! -x "$AE" ]; then
    echo "  [SKIP] c_import_union: build/ae not built"
    exit 0
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
cd "$SCRIPT_DIR" || exit 1

if ! "$AE" build probe.ae -o "$tmpdir/probe" >"$tmpdir/build.log" 2>&1; then
    echo "  [FAIL] c_import_union: ae build failed"
    sed 's/^/    /' "$tmpdir/build.log" | head -25
    exit 1
fi
if ! "$tmpdir/probe$EXE" >"$tmpdir/run.out" 2>&1; then
    echo "  [FAIL] c_import_union: the program failed"
    sed 's/^/    /' "$tmpdir/run.out" | head -10
    exit 1
fi
# 2.5f is 0x40200000.
want="size 16 first 2.5
bits 1075838976
tag 7"
if [ "$(cat "$tmpdir/run.out")" != "$want" ]; then
    echo "  [FAIL] c_import_union: expected:"
    echo "$want" | sed 's/^/    /'
    echo "  got:"
    sed 's/^/    /' "$tmpdir/run.out"
    exit 1
fi

# An extern union must be imported: Aether does not emit a union's layout.
cat > "$tmpdir/bad.ae" <<'AE'
extern union loose {
    a: int
}
main() { println("x") }
AE
if "$AE" build "$tmpdir/bad.ae" -o "$tmpdir/bad" >"$tmpdir/bad.log" 2>&1; then
    echo "  [FAIL] c_import_union: an extern union without @c_import built"
    exit 1
fi
if ! grep -q "extern union needs @c_import" "$tmpdir/bad.log"; then
    echo "  [FAIL] c_import_union: no diagnostic naming @c_import:"
    sed 's/^/    /' "$tmpdir/bad.log" | head -8
    exit 1
fi

echo "  [PASS] c_import_union: an imported union is spelled \`union\`, and one must be imported"
exit 0
