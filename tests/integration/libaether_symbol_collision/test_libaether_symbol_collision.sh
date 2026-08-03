#!/bin/sh
# Regression (#1366): a top-level function in the entry file must not collide
# with the bare C symbols libaether.a exports. This used to be a
# latent-until-upgrade break: code that compiled for months stopped linking the
# moment a stdlib release minted a bare C symbol the program already used as a
# function name, with no change to the program.
#
# Three shapes, because two different mechanisms are involved:
#   collide_same_sig  - name matches an extern the TU declares, signature agrees
#   collide_diff_sig  - same, signature disagrees (the in-TU declaration clash)
#   no_import_collide - name matches a libaether symbol the TU never declares,
#                       so only the generated stdlib symbol table catches it
#   fold_shadow       - the constant folder must not apply stdlib semantics to
#                       a name the program itself defines
#
# Each must build AND produce the right answer: a fix that merely silenced the
# linker while calling the wrong function would pass a build-only check.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] libaether_symbol_collision: ae not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
fail=0

# $1 = fixture basename, $2 = expected stdout
expect_builds_and_prints() {
    name="$1"
    want="$2"
    if ! "$AE" build "$SCRIPT_DIR/$name.ae" -o "$TMPDIR/$name" >"$TMPDIR/$name.log" 2>&1; then
        echo "  [FAIL] libaether_symbol_collision: $name.ae did not build"
        sed 's/^/        /' "$TMPDIR/$name.log" | head -12
        fail=1
        return
    fi
    got="$("$TMPDIR/$name" 2>&1)"
    if [ "$got" != "$want" ]; then
        echo "  [FAIL] libaether_symbol_collision: $name printed '$got', want '$want'"
        fail=1
    fi
}

expect_builds_and_prints collide_same_sig  "2 a"
expect_builds_and_prints collide_diff_sig  "2 42"
expect_builds_and_prints no_import_collide "x"
expect_builds_and_prints fold_shadow       "x"

# The user's colliding definition must be emitted under a renamed C symbol, so
# it can never bind to libaether's. Checked on the generated C rather than the
# binary so the assertion survives stripping.
"$AE" build "$SCRIPT_DIR/collide_same_sig.ae" -o "$TMPDIR/emit" --emit=csrc >"$TMPDIR/emit.log" 2>&1 || true
CFILE="$(find "$TMPDIR" -name '*.c' 2>/dev/null | head -1)"
if [ -n "$CFILE" ] && ! grep -q 'ae_string_replace_all' "$CFILE"; then
    echo "  [FAIL] libaether_symbol_collision: colliding definition was not renamed"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "  [PASS] libaether_symbol_collision: entry-file names no longer collide with libaether"
fi
exit $fail
