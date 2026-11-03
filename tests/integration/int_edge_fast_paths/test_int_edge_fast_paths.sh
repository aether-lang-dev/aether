#!/bin/sh
# #3524: integer `/`, `%`, `<<`, `>>` and a float converted to an integer
# follow Java's rules through helpers and masks, but a constant operand that
# keeps C defined keeps C's own operator, with no call and no mask: a
# divisor other than 0 and -1, a count inside the width, a float constant
# inside the range (one outside it is its saturated limit). A file-scope
# constant's initializer calls nothing either, -1 included. The compiler's
# own fold of LONG_MIN / -1 wraps, as the runtime does, and warns.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT" && AETHERC="$AETHERC$EXE_EXT"
NAME=int_edge_fast_paths

if [ ! -x "$AETHERC" ] && [ ! -x "$AETHERC.exe" ]; then
    echo "  [SKIP] $NAME: $AETHERC not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
fail() { echo "  [FAIL] $NAME: $1"; [ -n "${2:-}" ] && [ -f "$2" ] && sed 's/^/    /' "$2" | head -12; exit 1; }

"$AETHERC" "$SCRIPT_DIR/fast_paths.ae" "$TMPDIR/out.c" > "$TMPDIR/aetherc.log" 2>&1 ||
    fail "aetherc refused fast_paths.ae" "$TMPDIR/aetherc.log"

# body NAME: the C of the function NAME, from its definition to its end.
body() {
    awk -v fn="$1" '$0 ~ ("^[a-z0-9_]+ " fn "[(]") && $0 ~ /[{]$/ { on = 1 } on { print } on && /^}/ { exit }' "$TMPDIR/out.c"
}
plain() {
    b=$(body "$1")
    [ -n "$b" ] || fail "no C for $1" "$TMPDIR/out.c"
    case "$b" in *_ae_div_*|*_ae_mod_*|*_ae_f2*|*"& 31"*|*"& 63"*)
        echo "$b" > "$TMPDIR/body.txt"; fail "$1 has a helper call or a mask: $2" "$TMPDIR/body.txt" ;;
    esac
    printf '%s' "$b" | grep -q -- "$3" || { echo "$b" > "$TMPDIR/body.txt"; fail "$1 lacks '$3'" "$TMPDIR/body.txt"; }
}
checked() {
    b=$(body "$1")
    printf '%s' "$b" | grep -q -- "$2" || { echo "$b" > "$TMPDIR/body.txt"; fail "$1 lacks '$2'" "$TMPDIR/body.txt"; }
}

plain cdiv "a constant divisor" "a / 7"
plain cmod "a constant divisor" "a % 7"
plain cshl "a count inside the width" "<< 3"
plain cshr "a count inside the width" "a >> 3"
plain cfloat "a float constant inside the range" "2.5"
plain cbig "a float constant past the range" "2147483647"
checked vdiv "_ae_div_i32("
checked vshl "& 31)"
checked vfloat "_ae_f2i32("

# A file-scope constant's initializer stays a constant expression.
grep -E '(ae_const_)?(WRAP|REM|SHIFTED) = ' "$TMPDIR/out.c" > "$TMPDIR/consts.txt"
[ "$(wc -l < "$TMPDIR/consts.txt" | tr -d ' ')" -ge 3 ] || fail "constants not found" "$TMPDIR/out.c"
if grep -q "_ae_" "$TMPDIR/consts.txt"; then fail "a constant's initializer calls a helper" "$TMPDIR/consts.txt"; fi

grep -q "long constant expression overflows 64 bits and wraps to -9223372036854775808" "$TMPDIR/aetherc.log" ||
    fail "the fold of LONG_MIN / -1 did not warn of the wrap" "$TMPDIR/aetherc.log"

out=$("$AE" run "$SCRIPT_DIR/fast_paths.ae" 2>"$TMPDIR/run.log") || fail "fast_paths.ae did not run" "$TMPDIR/run.log"
want="7 1 40 -8 2 2147483647
7 2 -2147483648 -9223372036854775808
-2147483648 0 256"
[ "$out" = "$want" ] || { printf '%s\n' "$out" > "$TMPDIR/got.txt"; fail "wrong values" "$TMPDIR/got.txt"; }

echo "  [PASS] $NAME: constant operands keep C's operators; the rest go through the checks"
