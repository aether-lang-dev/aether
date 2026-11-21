#!/bin/sh
# A local named like a selectively imported constant or function is that
# local, whether or not the early inference pass can type its value yet.
#
# The walk bound a local only once it had a type for the value, so with
# `LIMIT = pick(xs)` (a result inferred from pick's body) the read
# `g = LIMIT` took the imported constant's string type, and the checker
# refused the program with E0200 "Type mismatch in variable initialization".
# The imported function is a guard: a function's entry never typed a read.
# Expected output: both locals read the Box back (3), and the imports still
# work.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run --lib lib main.ae 2>&1 )
rc=$?
got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
if [ "$rc" -ne 0 ] || [ "$got" != "3 3 limit w1" ]; then
    echo "  [FAIL] untyped_local_shadows_import: expected '3 3 limit w1' (rc 0), got rc=$rc:"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi
echo "  [PASS] untyped_local_shadows_import: an untyped local shadows an imported name"
exit 0
