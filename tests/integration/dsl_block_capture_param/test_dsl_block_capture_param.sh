#!/bin/sh
# A parameter that a closure in a trailing block captures, or that the block
# passes on as a value, is the parameter, also when the block's receiver has
# a function of the same name (#333). aether-ui's `reorderable(cb: fn)` beside
# a DSL `cb` setter took the setter for the parameter: the closure read
# `gui_cb` where it meant its captured `cb`, and the C compiler stopped at
# "incompatible integer to pointer conversion" (sae's tests/check_gate.sh
# item iv kept every such name apart because of it). The program's own
# bindings now shadow the receiver.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] dsl_block_capture_param: ae not built"; exit 0; }

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="$ROOT" "$AE" run --lib lib main.ae 2>&1 )
rc=$?
want="program cb 7 panel p program cb value 8 panel q"
got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
if [ "$rc" -ne 0 ] || [ "$got" != "$want" ]; then
    echo "  [FAIL] dsl_block_capture_param: expected '$want' (rc 0), got rc=$rc:"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi
echo "  [PASS] dsl_block_capture_param: a captured parameter is not the receiver's function"
exit 0
