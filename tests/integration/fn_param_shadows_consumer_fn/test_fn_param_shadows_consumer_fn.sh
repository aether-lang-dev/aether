#!/bin/sh
# #1657: a FUNCTION PARAMETER must shadow a same-named function defined in a
# DIFFERENT, CONSUMING module.
#
# Sibling of #1606, which fixed the closure-parameter case in the module
# renamer. This is the codegen half, and it survived that fix.
#
# After module merging, one program holds every module's functions — including
# non-exported ones from the consuming app. Codegen asked "is there a top-level
# function with this name?" to decide whether an argument was a bare function
# needing an env-ignoring adapter, and never checked whether the name was
# really a local binding. So a library's `btn(on_press: fn)` had its PARAMETER
# replaced by an adapter for the app's unrelated `on_press(view, x, y)`:
#
#     void* boxed = ui_box_it((_AeClosure){
#         .fn = (void(*)(void))_aether_bare_adapter_on_press, .env = NULL });
#
# The caller's closure was discarded and `.env = NULL` took its captures with
# it. Silent: the C is well-formed, the call returns, and nothing happens. In
# aether-ui this presented as a button that connected, fired, and never moved
# the model — and eventually segfaulted in GLib, a 3-arg adapter reached
# through a 0-arg signature.
#
# The library's PARAMETER NAMES were effectively part of its public API.
#
# Three properties are pinned, in increasing order of how legible the failure
# would be:
#
#   1. the emitted C passes the PARAMETER, not an adapter — the direct check,
#   2. the library's C is IDENTICAL whether or not the consumer defines a
#      colliding name — the property that actually matters to a library author,
#   3. the program RUNS and the captured variable survives — without this a
#      future regression shows up only as a callback that silently does
#      nothing, which is exactly how this hid.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMP="${TMPDIR:-/tmp}/ae_1657_$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

fail() { echo "  FAIL: $1"; exit 1; }

cd "$SCRIPT_DIR"

# --- 1. the colliding build must pass the parameter -------------------------
"$AE" build --emit=csrc probe.ae -o "$TMP/probe" >"$TMP/log" 2>&1 \
    || { cat "$TMP/log"; fail "probe.ae did not build"; }

# Assert on the adapter's USE, not its mere presence. The discovery walk still
# DECLARES an adapter for the consumer's on_press (it is a genuine bare
# function, and a speculative declaration is dead code the C compiler drops).
# The defect was the SUBSTITUTION at the call site, so that is what is pinned;
# checking for the declaration would fail on harmless output and teach the next
# person to weaken the test.
if grep -q 'bare_adapter_on_press, .env = NULL' "$TMP/probe.c"; then
    echo "  offending line:"
    grep -n 'bare_adapter_on_press, .env = NULL' "$TMP/probe.c" | head -3 | sed 's/^/    /'
    fail "the consumer's on_press replaced btn's parameter (#1657)"
fi
grep -q 'ui_box_it(on_press)' "$TMP/probe.c" \
    || fail "btn does not pass its own parameter to box_it"

# --- 2. the library's C must not depend on the consumer's names -------------
"$AE" build --emit=csrc probe_control.ae -o "$TMP/ctrl" >"$TMP/log2" 2>&1 \
    || { cat "$TMP/log2"; fail "probe_control.ae did not build"; }

# Compare just the library function's body: the rest of the file legitimately
# differs (the consumer's own function is named differently).
# Match the DEFINITION (it has a parameter list with names, so `_ctx` appears)
# rather than the forward declaration, and do not hardcode the return type.
sed -n '/ui_btn(void\* _ctx/,/^}/p' "$TMP/probe.c" > "$TMP/a.txt"
sed -n '/ui_btn(void\* _ctx/,/^}/p' "$TMP/ctrl.c"  > "$TMP/b.txt"
[ -s "$TMP/a.txt" ] || fail "could not locate ui_btn in the emitted C"
# cksum, not cmp: the Windows MSYS2 CI shell ships no diffutils.
[ "$(cksum < "$TMP/a.txt")" = "$(cksum < "$TMP/b.txt")" ] \
    || { echo "  colliding build:"; sed 's/^/    /' "$TMP/a.txt";
         echo "  control build:";   sed 's/^/    /' "$TMP/b.txt";
         fail "ui_btn's emitted C depends on the CONSUMER's function names"; }

# --- 3. it must actually run, with captures intact --------------------------
out="$("$AE" run probe.ae 2>&1)" || { echo "$out"; fail "probe.ae did not run"; }
echo "$out" | grep -q 'closure ran with captured-value' \
    || { echo "$out" | sed 's/^/    /'; fail "the closure did not run, or lost its captured variable"; }
echo "$out" | grep -q 'must NOT be what btn boxes' \
    && fail "the CONSUMER's on_press ran instead of the caller's closure"

echo "  PASS: fn_param_shadows_consumer_fn: a parameter wins over a consuming module's function (#1657)"
