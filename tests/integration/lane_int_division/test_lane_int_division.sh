#!/bin/sh
# #3524: `/` and `%` on integer lanes follow the scalar rules lane by lane
# through the _ae_<lanes>_div_checked / _mod_checked helpers, but a constant
# scalar divisor other than 0 and -1 keeps C's own vector operator (or the
# eight-lane _ae_i32x8_div / _mod), with no check: no lane can be 0 or -1
# there, and the C compiler turns it into a multiply per lane. A constant
# that is 0 or -1 in the lane's width (65536 in a 16-bit lane) is checked.
#
# tests/regression/test_lane_int_division.ae checks the values and the
# panics in the default build, where an i32x8 is two four-lane halves; where
# the CPU has AVX2, this runs it again built with `-mavx2 -O2`, where an
# i32x8 is one 256-bit vector and its helpers take their native bodies.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT" && AETHERC="$AETHERC$EXE_EXT"
NAME=lane_int_division

if [ ! -x "$AETHERC" ] && [ ! -x "$AETHERC.exe" ]; then
    echo "  [SKIP] $NAME: $AETHERC not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
export AETHER_HOME="$ROOT"
fail() { echo "  [FAIL] $NAME: $1"; [ -n "${2:-}" ] && [ -f "$2" ] && sed 's/^/    /' "$2" | head -12; exit 1; }

"$AETHERC" "$SCRIPT_DIR/fast_paths.ae" "$TMPDIR/out.c" > "$TMPDIR/aetherc.log" 2>&1 ||
    fail "aetherc refused fast_paths.ae" "$TMPDIR/aetherc.log"

# body NAME: the C of the function NAME, from its definition to its end.
body() {
    awk -v fn="$1" '$0 ~ ("^[A-Za-z0-9_]+ " fn "[(]") && $0 ~ /[{]$/ { on = 1 } on { print } on && /^}/ { exit }' "$TMPDIR/out.c"
}
plain() {
    b=$(body "$1")
    [ -n "$b" ] || fail "no C for $1" "$TMPDIR/out.c"
    case "$b" in *_checked*)
        echo "$b" > "$TMPDIR/body.txt"; fail "$1 calls a checked helper: $2" "$TMPDIR/body.txt" ;;
    esac
    printf '%s' "$b" | grep -q -F -- "$3" || { echo "$b" > "$TMPDIR/body.txt"; fail "$1 lacks '$3'" "$TMPDIR/body.txt"; }
}
checked() {
    b=$(body "$1")
    printf '%s' "$b" | grep -q -F -- "$2" || { echo "$b" > "$TMPDIR/body.txt"; fail "$1 lacks '$2'" "$TMPDIR/body.txt"; }
}

plain cdiv "a constant divisor" "v / ((int32_t)(4))"
plain cmod16 "a constant divisor" "v % ((int16_t)(3))"
plain cdiv64 "a constant divisor" "v / ((int64_t)(7))"
plain cdiv8 "a constant divisor" "_ae_i32x8_div(v, "
checked vdiv "_ae_i32x4_div_checked(v, w, "
checked sdiv "_ae_i32x4_div_checked(v, "
checked zdiv "_ae_i32x4_div_checked(v, "
checked mdiv "_ae_i64x2_div_checked(v, "
checked vmod8 "_ae_i32x8_mod_checked(v, w, "
checked wdiv16 "_ae_i16x8_div_checked(v, "
checked vdiv "\"division_by_zero: division by zero at \""

out=$("$AE" run "$SCRIPT_DIR/fast_paths.ae" 2>"$TMPDIR/run.log" | tr -d '\r') ||
    fail "fast_paths.ae did not run" "$TMPDIR/run.log"
[ "$out" = "-10 1 21" ] || { printf '%s\n' "$out" > "$TMPDIR/got.txt"; fail "wrong values" "$TMPDIR/got.txt"; }

# The native eight-lane form, where this CPU runs it.
case "$(uname -m 2>/dev/null)" in
    x86_64|amd64|AMD64)
        cat > "$TMPDIR/probe.c" <<'C'
int main(void) { __builtin_cpu_init(); return __builtin_cpu_supports("avx2") ? 0 : 1; }
C
        if command -v gcc >/dev/null 2>&1 && gcc -o "$TMPDIR/probe" "$TMPDIR/probe.c" 2>/dev/null &&
            "$TMPDIR/probe"; then
            mkdir -p "$TMPDIR/avx"
            cp "$ROOT/tests/regression/test_lane_int_division.ae" "$TMPDIR/avx/"
            printf '[package]\nname = "lane_int_division"\nversion = "0.1.0"\n\n[build]\ncflags = "-mavx2 -O2"\n' \
                > "$TMPDIR/avx/aether.toml"
            (cd "$TMPDIR/avx" && "$AE" build test_lane_int_division.ae -o "$TMPDIR/avx/t" \
                > "$TMPDIR/avx/build.log" 2>&1) || fail "the -mavx2 build failed" "$TMPDIR/avx/build.log"
            avx=$("$TMPDIR/avx/t" 2>&1 | tr -d '\r')
            [ "$avx" = "lane division: ok" ] ||
                { printf '%s\n' "$avx" > "$TMPDIR/avx.txt"; fail "the -mavx2 build gave other results" "$TMPDIR/avx.txt"; }
            echo "  [PASS] $NAME: constant divisors keep C's operators, the rest are checked; the -mavx2 eight-lane form agrees"
            exit 0
        fi
        ;;
esac
echo "  [PASS] $NAME: constant divisors keep C's operators, the rest are checked (no AVX2 here: the eight-lane native form was not run)"
