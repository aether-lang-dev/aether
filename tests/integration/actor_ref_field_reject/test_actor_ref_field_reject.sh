#!/bin/sh
# #3333: a field read through a bare `actor_ref` passed the type checker
# and failed in the C compiler (`request for member 'n' in something not a
# structure or union`): a bare reference is a `void*` and names no actor.
# The checker now refuses it with a hint to type the reference, and refuses
# a typed reference naming a state its actor does not have, which also
# reached C. A typed reference to a real state is accepted and runs.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
AE="$ROOT/build/ae"
[ -n "${EXE_EXT:-}" ] && AE="$AE$EXE_EXT" && AETHERC="$AETHERC$EXE_EXT"
NAME=actor_ref_field_reject

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
fail() { echo "  [FAIL] $NAME: $1"; [ -f "$2" ] && sed 's/^/    /' "$2" | head -12; exit 1; }

# refused FILE PATTERN: the checker refuses FILE with an error containing
# PATTERN, and writes no C.
refused() {
    if "$AETHERC" "$SCRIPT_DIR/$1" "$TMPDIR/out.c" >"$TMPDIR/err.log" 2>&1; then
        fail "aetherc accepted $1" "$TMPDIR/err.log"
    fi
    grep -q "$2" "$TMPDIR/err.log" || fail "$1: no error containing '$2'" "$TMPDIR/err.log"
    if grep -q -E "request for member|not a structure or union" "$TMPDIR/err.log"; then
        fail "$1 reached the C compiler" "$TMPDIR/err.log"
    fi
}
refused bare_field.ae "a bare \`actor_ref\` has no field 'n'"
grep -q "actor_ref\[ActorName\]" "$TMPDIR/err.log" || fail "the hint does not say to type the reference" "$TMPDIR/err.log"
refused unknown_state.ae "actor 'Counter' has no state field 'count'"

out=$("$AE" run "$SCRIPT_DIR/typed_field.ae" 2>"$TMPDIR/run.log") || fail "typed_field.ae did not run" "$TMPDIR/run.log"
[ "$out" = "1 1" ] || { echo "    got: $out"; fail "typed_field.ae printed the wrong state"; }

echo "  [PASS] $NAME: a bare reference's field and an unknown state are refused; a typed one runs"
