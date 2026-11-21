#!/bin/sh
# #333's shadowing rule: in a trailing block passed to a module's function,
# a bare name the program binds itself (its own function, a local, a
# closure bound to a local, a parameter) is the program's, and only a name
# the program does not bind falls back to the receiver's `<receiver>_<name>`.
#
# The receiver was asked first, ahead of everything outside the block, so
# `child.run(h) callback { say(1, 2, 3) }` beside the program's three-argument
# `say` was "Function 'child.say' expects 2 argument(s), got 3", and a
# program's `tag(a)` of the module's own arity built and silently called the
# module's. A local `label` read in the block was the module's function
# `label`, and a parameter likewise.
#
#   main.ae        the program's names win, in each of those shapes, in a
#                  nested block, a builder's block and a module's own body
#   fallback.ae    where the program binds nothing, the receiver's (the
#                  innermost first), through an import alias too
#   alias.ae       the program's function beside a receiver imported `as`
#   local_call.ae  a non-callable local hides the receiver's function, as it
#                  hides the program's own outside a block: refused by name

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] dsl_receiver_program_shadows: ae not built"; exit 0; }

fails=0

# expect <file> <expected output, lines joined by spaces>
expect() {
    out=$( cd "$SCRIPT_DIR" && AETHER_HOME="$ROOT" "$AE" run --lib lib "$1" 2>&1 )
    rc=$?
    got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
    if [ "$rc" -ne 0 ] || [ "$got" != "$2" ]; then
        echo "  [FAIL] dsl_receiver_program_shadows: $1: expected '$2' (rc 0), got rc=$rc:"
        echo "$out" | head -20 | sed 's/^/          /'
        fails=$((fails + 1))
    fi
}

expect main.ae "main.tag 1 main.say 6 var local param p main.closure 2 main.tag 3 main.closure 4 main.say 12 main.opt o build b opt=<unset> user.tag 40 main.tag 9 dsl.tag 10"
expect fallback.ae "dsl.tag 1 dsl.say 3 dsl.label inner.tag 2 inner.note 3 dsl.say 4 build b opt=o dsl.tag 4"
expect alias.ae "main.tag 1 dsl.say 2"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="$ROOT" "$AE" build --lib lib local_call.ae -o /dev/null 2>&1 )
if [ $? -eq 0 ]; then
    echo "  [FAIL] dsl_receiver_program_shadows: local_call.ae built; a local named tag must hide dsl.tag"
    fails=$((fails + 1))
elif ! printf '%s\n' "$out" | grep -q "'tag' is a local here, which hides the block receiver's function 'dsl.tag'"; then
    echo "  [FAIL] dsl_receiver_program_shadows: local_call.ae refused, but not by name:"
    echo "$out" | head -10 | sed 's/^/          /'
    fails=$((fails + 1))
fi

[ "$fails" -eq 0 ] || exit 1
echo "  [PASS] dsl_receiver_program_shadows: the program's own names come before a block's receiver"
exit 0
