#!/bin/sh
# Deeply nested source is refused, not a crash.
#
# The recursive-descent parser had no depth bound, so nesting deep enough
# exhausted the C stack and aetherc died of SIGSEGV with no diagnostic.
# Measured before the fix: ~2000 nested `(` or `if`, ~4000 nested `{`. Found by
# fuzzing; a user hits it with generated or pathological source, and a crash
# tells them nothing about what to change.
#
# Asserts three things, because only the first is obvious:
#   1. no signal death at depths far past the old crash point
#   2. exactly ONE error, not one per remaining token: the guard returns
#      without consuming input, so the block loop's force-advance would
#      otherwise emit an error for every token left in the file
#   3. ordinary shallow syntax errors still recover as before

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
[ -x "$AETHERC" ] || { echo "  [SKIP] parse_depth_guard: build/aetherc missing"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

gen() { # gen <kind> <depth> <out>
    python3 -c "
import sys
kind, d, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
if   kind == 'brace': s = 'main() {' + '{'*d + '}'*d + '}'
elif kind == 'paren': s = 'main() { x = ' + '('*d + '1' + ')'*d + ' }'
elif kind == 'neg':   s = 'main() { x = ' + '-'*d + '1 }'
else:                 s = 'main() { ' + 'if 1 {'*d + '}'*d + ' }'
open(out,'w').write(s + '\n')
" "$1" "$2" "$3"
}

for kind in brace paren neg ifs; do
    for depth in 1000 5000 20000; do
        gen "$kind" "$depth" "$TMP/deep.ae"
        set +e
        timeout 30 "$AETHERC" "$TMP/deep.ae" "$TMP/out.c" > "$TMP/log" 2>&1
        rc=$?
        set -e
        if [ "$rc" -gt 128 ]; then
            echo "  [FAIL] parse_depth_guard: $kind at depth $depth died of signal $((rc - 128))"
            exit 1
        fi
        if [ "$rc" -eq 124 ]; then
            echo "  [FAIL] parse_depth_guard: $kind at depth $depth timed out"
            exit 1
        fi
    done
done

# One error, not a flood. A cascade means the guard stopped consuming input
# without stopping the parse.
gen paren 5000 "$TMP/deep.ae"
set +e
"$AETHERC" "$TMP/deep.ae" "$TMP/out.c" > "$TMP/log" 2>&1
set -e
n=$(grep -c '^error' "$TMP/log" || true)
if [ "$n" != "1" ]; then
    echo "  [FAIL] parse_depth_guard: expected exactly 1 error, got $n"
    grep '^error' "$TMP/log" | head -3 | sed 's/^/        /'
    exit 1
fi
grep -q "nested too deeply" "$TMP/log" || {
    echo "  [FAIL] parse_depth_guard: the error does not name the depth limit"
    head -3 "$TMP/log" | sed 's/^/        /'
    exit 1
}

# Shallow malformed input keeps its existing recovery: one error, no crash.
printf 'deep() { x = ((((((((((1 }\nmain() { println("hi") }\n' > "$TMP/shallow.ae"
set +e
"$AETHERC" "$TMP/shallow.ae" "$TMP/out.c" > "$TMP/log2" 2>&1
rc=$?
set -e
[ "$rc" -le 128 ] || { echo "  [FAIL] parse_depth_guard: shallow syntax error crashed"; exit 1; }
n2=$(grep -c '^error' "$TMP/log2" || true)
[ "$n2" = "1" ] || {
    echo "  [FAIL] parse_depth_guard: shallow error recovery changed ($n2 errors, expected 1)"
    exit 1
}

# Real code is nowhere near the limit: the deepest nesting in this repository
# is 22 braces, so a file well past that must still compile.
python3 -c "
d = 60
open('$TMP/ok.ae','w').write('main() {\n' + '    if true {\n'*d + '        println(\"deep\")\n' + '    }\n'*d + '}\n')
"
"$AETHERC" "$TMP/ok.ae" "$TMP/out.c" > "$TMP/log3" 2>&1 || {
    echo "  [FAIL] parse_depth_guard: 60 levels of legitimate nesting was rejected"
    head -5 "$TMP/log3" | sed 's/^/        /'
    exit 1
}

echo "  [PASS] parse_depth_guard: deep nesting is refused with one error, not a crash"
exit 0
