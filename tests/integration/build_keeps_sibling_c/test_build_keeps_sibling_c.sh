#!/bin/sh
# ae build's generated C is an intermediate. It used to be written beside the
# output (<-o>.c, or <base>.c in the current directory) and removed after,
# which is the name a user's own C file has: `ae build y.ae -o y` beside a y.c
# wrote over it and removed it, `-o x` did the same to an `@source("x.c")`,
# and `--emit=obj` left the generated C behind as <base>.c. It now goes to the
# temp directory; AE_KEEP_C=1 still keeps it beside the output (#2885).
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="${AE:-$ROOT/build/ae}"
[ -x "$AE" ] || { echo "  [SKIP] build_keeps_sibling_c: ae not built"; exit 0; }
. "$ROOT/tests/lib/same_bytes.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cd "$TMP" || exit 1
fails=0
bad() { echo "    $1"; fails=$((fails + 1)); }

printf 'main() { println("y") }\n' > y.ae
printf 'not generated: the user wrote this\n' > y.c
cp y.c y.c.mine
"$AE" build y.ae -o y > build.log 2>&1 || { bad "build of y.ae failed"; sed 's/^/      /' build.log | tail -5; }
same_bytes y.c y.c.mine || bad "-o y wrote over (or removed) the user's y.c"
[ "$(./y 2>/dev/null)" = y ] || bad "the y program does not run"

printf 'int helper(void) { return 42; }\n' > x.c
cp x.c x.c.mine
printf '@source("x.c")\nextern helper() -> int\nmain() { println("helper=${helper()}") }\n' > x.ae
"$AE" build x.ae -o x > build.log 2>&1 || { bad "build of x.ae failed"; sed 's/^/      /' build.log | tail -5; }
same_bytes x.c x.c.mine || bad "-o x wrote over (or removed) the @source x.c"
[ "$(./x 2>/dev/null)" = helper=42 ] || bad "the x program does not run"

mkdir obj && cd obj && cp ../y.ae . || exit 1
"$AE" build --emit=obj y.ae -o y.o > build.log 2>&1 || { bad "--emit=obj failed"; sed 's/^/      /' build.log | tail -5; }
[ -f y.o ] || bad "--emit=obj made no y.o"
[ -e y.c ] && bad "--emit=obj left its generated C behind as y.c"
cd ..

mkdir keep && cd keep && cp ../y.ae k.ae || exit 1
AE_KEEP_C=1 "$AE" build k.ae -o kept > build.log 2>&1 || { bad "AE_KEEP_C build failed"; sed 's/^/      /' build.log | tail -5; }
[ -f kept.c ] || bad "AE_KEEP_C=1 did not keep the generated C as kept.c"
cd ..

if [ "$fails" -ne 0 ]; then
    echo "  [FAIL] build_keeps_sibling_c: $fails problem(s)"
    exit 1
fi
echo "  [PASS] build_keeps_sibling_c: a user's y.c and an @source x.c survive -o, --emit=obj leaves no C, AE_KEEP_C keeps it"
