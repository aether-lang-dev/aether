#!/bin/sh
# A string given to a header-defined struct's string field is given back
# (#3567): the field's reference is recorded by the pointer C holds, and
# string.release_kept gives it back. See prog.ae.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cd "$SCRIPT_DIR" || exit 1

if ! "$AE" build prog.ae -o "$tmpdir/prog" >"$tmpdir/build.log" 2>&1; then
    echo "  [FAIL] c_import_string_field_given_back: ae build failed"
    sed 's/^/    /' "$tmpdir/build.log" | head -25
    exit 1
fi
if ! "$tmpdir/prog" > "$tmpdir/run.out" 2>&1; then
    echo "  [FAIL] c_import_string_field_given_back:"
    sed 's/^/    /' "$tmpdir/run.out" | head -10
    exit 1
fi
if ! grep -q "^PASS: a string given to a C field is given back" "$tmpdir/run.out"; then
    echo "  [FAIL] c_import_string_field_given_back: no PASS line"
    sed 's/^/    /' "$tmpdir/run.out" | head -10
    exit 1
fi
echo "  [PASS] c_import_string_field_given_back"
exit 0
