#!/bin/sh
# Regression (#2862): `ae examples` takes every example, however many.
#
# It read the example list into 512 rows: #2862 made the 513th example on
# an error each, counted as a failure, while the header still said
# "Building 512 example(s)". The list grows now, and every path built from
# an example's is whole. 520 sources under a lib/ directory (which the
# command counts and skips, as module files) and one example to build give
# a header of 521 and one example built, none failed.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
EXE=""
[ -x "$ROOT/build/ae" ] || EXE=".exe"
AE="$ROOT/build/ae$EXE"
if [ ! -x "$AE" ]; then
    echo "  [SKIP] ae_examples_many: ae not built"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/ex/lib"
i=0
while [ $i -lt 520 ]; do
    printf 'f%d() -> int {\n    return %d\n}\n' "$i" "$i" > "$TMP/ex/lib/m$i.ae"
    i=$((i + 1))
done
printf 'main() {\n    println("hello")\n}\n' > "$TMP/ex/hello.ae"

out="$(cd "$TMP" && AETHER_HOME="$ROOT" "$AE" examples ex 2>&1)"
rc=$?
fail=0
if ! printf '%s' "$out" | grep -q "Building 521 example(s)"; then
    echo "  [FAIL] ae_examples_many: the header does not count all 521 examples"
    printf '%s\n' "$out" | head -3 | sed 's/^/        /'
    fail=1
fi
if [ $rc -ne 0 ] || ! printf '%s' "$out" | grep -q "1 passed, 0 failed, 1 total"; then
    echo "  [FAIL] ae_examples_many: the one example did not build alone (rc=$rc)"
    printf '%s\n' "$out" | tail -4 | sed 's/^/        /'
    fail=1
fi
if [ "$fail" -ne 0 ]; then exit 1; fi
echo "  [PASS] ae_examples_many: 521 examples listed, the one to build built"
exit 0
