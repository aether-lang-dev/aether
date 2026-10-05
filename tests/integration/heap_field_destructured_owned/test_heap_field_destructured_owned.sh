#!/bin/sh
# #2366: a heap string destructured from a tuple and stored in a struct field
# must end up owned by the field. The destructured local escapes into the
# field, and the destructure used to emit a bare `content = _tup._0;` for an
# escaped local, leaving `_heap_content` at 0. The field store then moved that
# 0 into `_heap_data`, so the struct's destructor never freed the string and
# nothing else did either.
#
# The leak is silent without a leak checker, so the ownership flag is asserted
# on the generated C; the program is also built and run to completion, which
# would abort on a double free if the flag were set twice.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT" && AETHERC="$AETHERC$EXE_EXT"
[ -x "$AE" ] || { echo "  [SKIP] heap_field_destructured_owned: build/ae missing"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

fail() {
    echo "  [FAIL] heap_field_destructured_owned: $1"
    exit 1
}

# 1. The destructure records that the escaped local owns the new value, and
#    the field store moves that ownership into the field's tracker.
"$AETHERC" "$SCRIPT_DIR/prog.ae" "$TMP/out.c" > "$TMP/gen.log" 2>&1 \
    || { sed 's/^/        /' "$TMP/gen.log" | head -8; fail "codegen failed"; }
if ! grep -q "content = _tup[0-9]*\._0; _heap_content = 1;" "$TMP/out.c"; then
    grep -n "content = _tup" "$TMP/out.c" | sed 's/^/        /' | head -4
    fail "the destructure left the escaped local's tracker unset, so nothing owns the string"
fi
grep -q "_heap_data = _heap_content; _heap_content = 0;" "$TMP/out.c" \
    || fail "the field store did not move the local's ownership into the field"

# 2. It runs to completion and prints each clip.
AETHER_HOME="$ROOT" "$AE" build "$SCRIPT_DIR/prog.ae" -o "$TMP/prog" > "$TMP/build.log" 2>&1 \
    || { sed 's/^/        /' "$TMP/build.log" | head -8; fail "build failed"; }
"$TMP/prog" > "$TMP/run.out" 2>&1 || { sed 's/^/        /' "$TMP/run.out" | head -8; fail "program aborted"; }
tr -d '\r' < "$TMP/run.out" > "$TMP/run.txt"
for want in clip-0 clip-1 clip-2 done; do
    grep -qx "$want" "$TMP/run.txt" || fail "output lacks '$want'"
done

echo "  [PASS] heap_field_destructured_owned"
