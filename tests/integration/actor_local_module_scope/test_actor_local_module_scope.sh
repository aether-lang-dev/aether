#!/bin/sh
# An actor's handler locals are the handler's, in its own module only.
#
# The early inference walk took actors through its generic path, which never
# unwound what it bound, so a handler's `f = spawn(Fetcher())` stayed in the
# table for every walk after the actor, of every function in every module
# merged after it. A module function binding its own `f` from a value that
# walk cannot type yet then read the handler's `f` in `g = f`:
#
#   - the program's Leader handler into lib/ctxwork's rotate, where `f` is
#     read through a pointer: E0302 "Unknown type 'Fetcher': the local 'g'
#     is declared of a type the program does not define", reported inside
#     the module (found in std/cryptography/sha2's process_block_512);
#   - lib/coord's Coord handler into lib/picker, imported after it: "Type
#     mismatch in variable initialization".
#
# The fix walks an actor as a scope of its own. Expected output: rotate moved
# 7 into h6 and added 1, coord answered, and pick handed the list back.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run --lib lib main.ae 2>&1 )
rc=$?
got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
if [ "$rc" -ne 0 ] || [ "$got" != "8 1 true" ]; then
    echo "  [FAIL] actor_local_module_scope: expected '8 1 true' (rc 0), got rc=$rc:"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi
echo "  [PASS] actor_local_module_scope: a handler's locals do not type another module's"
exit 0
