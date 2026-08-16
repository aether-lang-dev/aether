#!/bin/sh
# #1606: a CLOSURE PARAMETER must shadow a same-named module-level function.
#
# The module namespacing pass (rename_intra_module_refs) rewrites bare
# references to module functions into their `<ns>_<name>` spelling so the
# emitted C resolves. It skipped that rewrite for names shadowed by a local
# binding — but `collect_local_names` only knew about AST_PATTERN_VARIABLE /
# AST_VARIABLE_DECLARATION / AST_CONST_DECLARATION, and a closure parameter is
# an AST_CLOSURE_PARAM. Closures also established no scope of their own.
#
# So in a module defining `item()`, a closure `|item: ptr| { f(item) }` had its
# `item` rewritten to `<ns>_item`: the ADDRESS OF THE FUNCTION passed where the
# parameter's value belonged. It compiled cleanly, type-checked cleanly, and the
# callee then read a function pointer as data.
#
# Two properties are pinned here, and the first is the load-bearing one:
#
#   1. the value survives — printing it yields the string that was passed in,
#      not the bytes of a function body,
#   2. the emitted C names the PARAMETER (`item`) in argument position, not the
#      prefixed module function (`uix_item`).
#
# (2) is what makes a regression legible: without it a future breakage shows up
# only as garbage output, which is exactly how this bug hid for so long. In
# aether-ui the same defect segfaulted table_demo at startup inside
# aether_string_data — verified A/B on commit e6feacc: SIGSEGV with the unfixed
# compiler, runs clean with the fix.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"
[ -n "${EXE_EXT:-}" ] && { AE="$AE$EXE_EXT"; AETHERC="$AETHERC$EXE_EXT"; }

[ -x "$AE" ] || { echo "  [SKIP] closure_param_shadows_module_fn: ae not built"; exit 0; }

cd "$ROOT" || exit 1
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail() {
    echo "  [FAIL] closure_param_shadows_module_fn: $1"
    [ -n "$2" ] && [ -f "$2" ] && sed 's/^/        /' "$2" | head -12
    exit 1
}

# ---- Property 1: the parameter's VALUE reaches the callee -----------------
AETHER_LIB_DIR="$SCRIPT_DIR/lib" "$AE" run "$SCRIPT_DIR/probe.ae" \
    >"$TMP/out.log" 2>&1 || fail "probe did not run" "$TMP/out.log"

grep -q "cell -> REAL-ROW" "$TMP/out.log" \
    || fail "closure parameter did not survive; the module function shadowed it" "$TMP/out.log"

# The failure mode was printing a function body's bytes. Catch any recurrence
# even if the marker string somehow appeared too.
if grep -qE "cell -> .*[^R][^E][^A][^L]" "$TMP/out.log" && ! grep -q "cell -> REAL-ROW" "$TMP/out.log"; then
    fail "unexpected cell payload — function bytes leaking again?" "$TMP/out.log"
fi

# ---- Property 2: the emitted C names the parameter, not the module fn -----
AETHER_LIB_DIR="$SCRIPT_DIR/lib" "$AETHERC" "$SCRIPT_DIR/probe.ae" "$TMP/probe.c" \
    >"$TMP/gen.log" 2>&1 || fail "aetherc failed" "$TMP/gen.log"

# The call must pass `item` (the closure's own parameter).
grep -q "uix_invoke_cell(cell, item," "$TMP/probe.c" \
    || fail "emitted C does not pass the closure parameter in argument position" "$TMP/probe.c"

# And must NOT pass the prefixed module function's address.
if grep -q "uix_invoke_cell(cell, uix_item," "$TMP/probe.c"; then
    fail "emitted C passes the module function uix_item instead of the parameter (#1606 regression)"
fi

# The module function itself must still be prefixed where it IS meant — the fix
# must not have disabled intra-module renaming wholesale.
grep -q "uix_item" "$TMP/probe.c" \
    || fail "module function lost its namespace prefix entirely — rename pass over-suppressed"

echo "  [PASS] closure_param_shadows_module_fn: closure param wins over same-named module fn (#1606)"
exit 0
