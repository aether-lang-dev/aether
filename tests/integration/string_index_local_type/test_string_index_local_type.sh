#!/bin/sh
# #3479: a local bound to one byte of a string (`c = s[0]`, `c = text[i]`)
# was left untyped by inference, and codegen defaulted it to int with
# "warning: unresolved type in codegen, defaulting to int", which fails
# `make check-examples` (warnings are errors there). The read is a byte, so
# the local is a byte.
#
# string_index_local.ae binds the read from a local, a parameter, a field, a
# returned string and *StringSeq / string[] elements, and checks the values
# (a high byte is 195, not -61). Building it must print no warning at all;
# running it must pass.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
export AETHER_HOME="$ROOT"

[ -x "$AE" ] || [ -x "$AE.exe" ] || { echo "  [SKIP] string_index_local_type: ae not built"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cp "$SCRIPT_DIR/string_index_local.ae" "$TMP/"

if ! (cd "$TMP" && "$AE" build string_index_local.ae -o prog > build.log 2>&1); then
    echo "  [FAIL] string_index_local_type: build failed"
    sed 's/^/        /' "$TMP/build.log" | head -20
    exit 1
fi
if grep -q "warning" "$TMP/build.log"; then
    echo "  [FAIL] string_index_local_type: the build warned"
    sed 's/^/        /' "$TMP/build.log" | head -20
    exit 1
fi
out="$(cd "$TMP" && ./prog 2>&1)"
case "$out" in
    *"PASS: string_index_local_type"*) ;;
    *) echo "  [FAIL] string_index_local_type: the program failed"
       echo "$out" | sed 's/^/        /'
       exit 1 ;;
esac
echo "  [PASS] string_index_local_type: s[i] binds a byte local with no warning, from every source"
