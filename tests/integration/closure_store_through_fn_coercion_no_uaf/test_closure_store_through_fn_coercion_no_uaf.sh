#!/bin/sh
# Regression: a capturing closure stored through the `fn -> ptr` coercion
# (`e.cb = h`, h an `fn` parameter, cb a `ptr` field) outlives the call that
# stored it (aether-ui asks/aether-closure-drain-through-fn-store.md).
#
# The escape walk counts a store into a field as the holder's keep (#2528),
# so the caller releases its own reference after the call
#   set_cb(a, _ad_0); if (_ad_0.env) _closure_env_0_free(...)
# which is right only if the holder took one. A `fn` field does (#2525); the
# box a `ptr` field gets did not, so that release freed the env.
#
# Two checks:
#   1. the generated C boxes a stored closure with a reference of the box's
#      own: `_aether_box_closure(_aether_closure_retain(h))`, never a bare
#      `_aether_box_closure(h)` (nor `(cl)` / `(kept)` for a local or an alias)
#   2. the probe, which invokes every stored handler after its setter has
#      returned, prints each handler's captured values and exits 0. Before
#      the fix it printed "(null) 2" on macOS and crashed on glibc.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
cd "$SCRIPT_DIR" || exit 1
fail=0

if ! AETHER_HOME="" "$AETHERC" probe.ae "$tmpdir/probe.c" >"$tmpdir/aetherc.log" 2>&1; then
    echo "  [FAIL] closure_store_through_fn_coercion_no_uaf: aetherc failed"
    sed 's/^/    /' "$tmpdir/aetherc.log" | head -20
    exit 1
fi
bare="$(grep -n -E '_aether_box_closure\((h|cl|kept)\)' "$tmpdir/probe.c")"
kept="$(grep -c -E '_aether_box_closure\(_aether_closure_retain\((h|cl|kept)\)\)' "$tmpdir/probe.c")"
if [ -n "$bare" ] || [ "$kept" -ne 4 ]; then
    echo "  [FAIL] a closure stored through fn -> ptr is boxed without a reference of its own ($kept retained):"
    printf '%s\n' "$bare" | sed 's/^/    /'
    fail=1
else
    echo "  [PASS] each store through fn -> ptr boxes the closure with a reference of its own"
fi

if ! AETHER_HOME="" "$AE" build probe.ae -o "$tmpdir/probe" >"$tmpdir/build.log" 2>&1; then
    echo "  [FAIL] closure_store_through_fn_coercion_no_uaf: ae build failed"
    sed 's/^/    /' "$tmpdir/build.log" | head -30
    exit 1
fi
"$tmpdir/probe" >"$tmpdir/run.log" 2>&1
rc=$?
expected='first 42
second 43
third 80
fourth local 4
fifth 39'
got="$(cat "$tmpdir/run.log")"
if [ "$rc" -eq 0 ] && [ "$got" = "$expected" ]; then
    echo "  [PASS] every stored handler runs after its setter returned, with its captures intact"
else
    echo "  [FAIL] stored handlers (exit $rc)"
    printf '%s\n' "--- expected" "$expected" "--- got" "$got" | sed 's/^/    /'
    fail=1
fi
# The other holders, direct and through an alias: a `fn` field, a global, a
# list and a map. They kept their own references before; this keeps them so.
if ! AETHER_HOME="" "$AE" build probe_forms.ae -o "$tmpdir/forms" >"$tmpdir/forms_build.log" 2>&1; then
    echo "  [FAIL] ae build probe_forms.ae"
    sed 's/^/    /' "$tmpdir/forms_build.log" | head -30
    exit 1
fi
"$tmpdir/forms" >"$tmpdir/forms.log" 2>&1
frc=$?
fexpected='field A 1
field-alias A 2
global A 3
global-alias A 4
list A 5
list-alias A 6
map A 7
map-alias A 8'
fgot="$(cat "$tmpdir/forms.log")"
if [ "$frc" -eq 0 ] && [ "$fgot" = "$fexpected" ]; then
    echo "  [PASS] fn field, global, list and map holders keep their closures, direct and aliased"
else
    echo "  [FAIL] fn field / global / list / map holders (exit $frc)"
    printf '%s\n' "--- expected" "$fexpected" "--- got" "$fgot" | sed 's/^/    /'
    fail=1
fi
exit "$fail"
