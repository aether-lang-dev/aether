#!/bin/sh
# #2890: compile-time refusal of a closure made in a receive arm that reads
# a state field its copy would not hold: an `fn[N]` (the copy takes none of
# the closures' references) and an array of structs that own strings (the
# copy takes none of the strings). The next store into the state would free
# what the closure reads. The error names the field, its type and the way
# out (read what is needed into arm locals first). The kinds a closure
# copies safely run in tests/regression/test_issue2890_arm_closure_state_copies.ae.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
[ -f "$AETHERC.exe" ] && AETHERC="$AETHERC.exe"

if [ ! -f "$AETHERC" ]; then
    echo "  [SKIP] closure_actor_state_read_reject: toolchain not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

fail() {
    echo "  [FAIL] closure_actor_state_read_reject: $1"
    exit 1
}

for case in "reads_fn_array fs" "reads_struct_array items"; do
    src="${case%% *}"
    field="${case#* }"
    if "$AETHERC" "$SCRIPT_DIR/$src.ae" "$TMPDIR/out.c" >"$TMPDIR/err.log" 2>&1; then
        fail "$src.ae was accepted: a closure reads state field '$field' it cannot copy"
    fi
    grep -q "reads state field '$field'" "$TMPDIR/err.log" ||
        { cat "$TMPDIR/err.log"; fail "$src.ae: the error does not name state field '$field'"; }
    grep -q "arm locals" "$TMPDIR/err.log" ||
        { cat "$TMPDIR/err.log"; fail "$src.ae: the error does not point at arm locals"; }
done

echo "  [PASS] closure_actor_state_read_reject: state an arm closure cannot copy is refused"
