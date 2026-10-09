#!/bin/sh
# Regression (#2684): a function whose result is not void and whose end
# control can reach is rejected (E0700), named, at its closing brace. Before
# this, `aetherc` and `ae check` accepted it and the generated C fell off the
# end of a non-void function: undefined behaviour, a string printed "(null)".
#
# reject_shapes.ae holds one function per shape that can fall off its end;
# each must be reported at the line of its closing brace, and nothing else
# may be. The shapes that return on every path are compiled and run by
# tests/regression/test_missing_return_accepted.ae.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] missing_return: ae not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
cd "$SCRIPT_DIR" || exit 1
fail=0
log="$TMPDIR/reject.log"

"$AE" check reject_shapes.ae >"$log" 2>&1
if [ $? -eq 0 ]; then
    echo "  [FAIL] missing_return: reject_shapes.ae passed ae check"
    fail=1
fi

# $1 = the name the error gives, $2 = the line of the closing brace.
expect() {
    if ! grep -A1 "E0700.* end of $1" "$log" | grep -q "reject_shapes.ae:$2:"; then
        echo "  [FAIL] missing_return: no E0700 for $1 at line $2"
        fail=1
    fi
}

expect "'trailing_value'" 18
expect "'if_no_else'" 24
expect "'else_if_no_else'" 32
expect "'else_falls'" 40
expect "'match_no_catchall'" 47
expect "'match_value_dropped'" 55
expect "'optional_some_only'" 61
expect "'while_can_end'" 71
expect "'while_true_break'" 80
expect "'labeled_break'" 91
expect "'for_range'" 99
expect "'switch_no_default'" 106
expect "'catch_falls'" 114
expect "'panic_on_one_path'" 120
expect "'defer_is_no_return'" 127
expect "'nested_if'" 137
expect "this clause of 'guarded'" 144
expect "this clause of 'clause_declared'" 152
expect "'unannotated'" 162
expect "'tuple_result'" 168
expect "'empty_body'" 171
expect "'trailing_block_falls'" 180
expect "'arrow_if'" 187
expect "a closure" 194
expect "a closure" 207

want=25
got=$(grep -c "E0700" "$log")
if [ "$got" -ne "$want" ]; then
    echo "  [FAIL] missing_return: $got E0700 errors, want $want"
    fail=1
fi
if grep "^error" "$log" | grep -qv "E0700"; then
    echo "  [FAIL] missing_return: an error other than E0700"
    fail=1
fi

# The help line names why control gets through.
for why in "\`msg\` on line 17 is not returned" \
           "the \`if\` on line 21 has no \`else\`" \
           "the \`match\` on line 43 has no \`_\` arm" \
           "the value of the \`match\` on line 51 is not returned" \
           "the loop on line 65 ends when its condition is false" \
           "the \`break\` on line 76 leaves the loop on line 74" \
           "the \`break\` on line 86 leaves the loop on line 83" \
           "the \`switch\` on line 102 has no \`default\`" \
           "the body is empty"; do
    if ! grep -qF "$why" "$log"; then
        echo "  [FAIL] missing_return: no help saying: $why"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    sed 's/^/        /' "$log" | head -60
    exit 1
fi
echo "  [PASS] missing_return: 25 shapes that fall off their end rejected at their closing brace"
exit 0
