#!/bin/sh
# An interpolation nested inside a print/println segment must build its
# string, not print it. The printf mode print/println use for their own
# interpolation used to stay up while the segments were generated, so
# `println("${takes("${base}/x")}")` printed `abc/x` to stdout on its own
# and passed printf's return count to `takes` as a pointer. The exact
# output is compared, since a stray print is the symptom.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] interp_nested_in_print: $AE not built"
    exit 0
fi

want='=== nested interpolation inside print ===
1: 5
2: 5
3: [abc/x]
4: ABC/X and 3
5: 7
6: 5
=== done ==='
got="$(AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/nested.ae" 2>&1)"
if [ "$got" = "$want" ]; then
    echo "  [PASS] interp_nested_in_print: nested interpolations build their strings"
    exit 0
fi
echo "  [FAIL] interp_nested_in_print: output differs"
printf '%s\n' "$got" | head -12 | sed 's/^/        /'
exit 1
