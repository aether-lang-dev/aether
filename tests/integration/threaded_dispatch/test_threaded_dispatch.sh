#!/bin/sh
# #2378: a `while <always true> { head; switch sel { ... } }` interpreter loop
# lowers to threaded dispatch (computed goto) under GCC/Clang, and to today's
# plain loop-and-switch elsewhere or with AETHER_NO_THREADED_DISPATCH defined.
#
#   1. The shape is recognised: the regression test's loops and the VM below
#      (case labels from an imported module, a head with its own fast path)
#      get dispatch tables and `goto *` jumps.
#   2. Loops outside the shape are left alone, and AETHER_EXPLAIN_THREADED=1
#      says why for each one.
#   3. Threaded and fallback builds of the same programs print the same thing.
#      The fallback is built with the macro from aether.toml [build] cflags;
#      `nm` confirms the table is gone, so the comparison is real.
#   4. Under --emit=lib, every threaded dispatch checks the call deadline, not
#      just the loop head.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"
AE="$ROOT/build/ae"
REG="$ROOT/tests/regression/test_threaded_dispatch.ae"

pass=0
fail=0
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT

ok() { echo "  [PASS] threaded_dispatch: $1"; pass=$((pass + 1)); }
bad() { echo "  [FAIL] threaded_dispatch: $1"; fail=$((fail + 1)); }

# --- 1. recognised -----------------------------------------------------------
if "$AETHERC" "$REG" "$tmp/reg.c" > "$tmp/reg.log" 2>&1; then
    tables=$(grep -c 'static void\* const _ae_td_tbl_' "$tmp/reg.c")
    jumps=$(grep -c 'goto \*_ae_td_tbl_' "$tmp/reg.c")
    # Nine loops: basic, with_default, head_continue, labeled, nested (two),
    # cleanups, wide, with_tail.
    if [ "$tables" = "9" ] && [ "$jumps" -gt 9 ]; then
        ok "the regression test's 9 dispatch loops are threaded ($jumps dispatch sites)"
    else
        bad "expected 9 dispatch tables and more than 9 jumps, got $tables and $jumps"
    fi
else
    bad "aetherc failed on the regression test"
    sed 's/^/        /' "$tmp/reg.log" | head -10
fi

mkdir -p "$tmp/td" "$tmp/sw"
cp -r "$SCRIPT_DIR/vm/." "$tmp/td/"
cp -r "$SCRIPT_DIR/vm/." "$tmp/sw/"
if (cd "$tmp/td" && "$AETHERC" main.ae "$tmp/vm.c" > "$tmp/vm.log" 2>&1) &&
   grep -q 'goto \*_ae_td_tbl_1\[' "$tmp/vm.c"; then
    ok "a VM with module-constant opcodes and a head fast path is threaded"
else
    bad "the VM fixture was not threaded"
    sed 's/^/        /' "$tmp/vm.log" | head -10
fi

# --- 2. not recognised -------------------------------------------------------
NEG="$SCRIPT_DIR/fixtures/not_threaded.ae"
AETHER_EXPLAIN_THREADED=1 "$AETHERC" "$NEG" "$tmp/neg.c" > "$tmp/neg.log" 2>&1
if [ "$(grep -c '_ae_td_' "$tmp/neg.c")" = "0" ]; then
    ok "loops outside the shape lower exactly as before"
else
    bad "a loop outside the shape was threaded"
fi
for why in "a case is a range" \
           "a case value is not an integer constant in 0..4095" \
           "a statement before the switch cannot be repeated" \
           "no arm continues the loop"; do
    if grep -q "note: dispatch loop not threaded: $why" "$tmp/neg.log"; then
        ok "explained: $why"
    else
        bad "no explanation: $why"
        sed 's/^/        /' "$tmp/neg.log" | head -10
    fi
done
# The `while i < 3` loop (a real condition) is not a dispatch loop at all,
# so it gets no note: 6 loops in the file, 5 notes (two out-of-range cases).
n=$(grep -c 'note: dispatch loop not threaded' "$tmp/neg.log")
if [ "$n" = "5" ]; then
    ok "only loops that look like dispatch loops are explained"
else
    bad "expected 5 notes, got $n"
fi
if [ "$(AETHER_EXPLAIN_THREADED= "$AETHERC" "$NEG" "$tmp/neg2.c" 2>&1 | grep -c 'not threaded')" = "0" ]; then
    ok "no notes unless AETHER_EXPLAIN_THREADED is set"
else
    bad "notes printed without AETHER_EXPLAIN_THREADED"
fi

# --- 3. threaded and fallback agree -----------------------------------------
printf '[build]\ncflags = "-DAETHER_NO_THREADED_DISPATCH"\n' > "$tmp/sw/aether.toml"
cp "$REG" "$tmp/td/reg.ae"
cp "$REG" "$tmp/sw/reg.ae"
cp "$NEG" "$tmp/td/neg.ae"
cp "$NEG" "$tmp/sw/neg.ae"
for v in td sw; do
    for p in main reg neg; do
        if ! (cd "$tmp/$v" && "$AE" build "$p.ae" -o "$p" > "$p.build.log" 2>&1); then
            bad "ae build $p ($v) failed"
            sed 's/^/        /' "$tmp/$v/$p.build.log" | head -10
            continue
        fi
        (cd "$tmp/$v" && "./$p" > "$p.out" 2>&1)
        echo $? > "$tmp/$v/$p.rc"
    done
done
check_same() {
    p="$1"; want="$2"
    td="$(cat "$tmp/td/$p.out" 2>/dev/null)"
    sw="$(cat "$tmp/sw/$p.out" 2>/dev/null)"
    if [ "$(cat "$tmp/td/$p.rc" 2>/dev/null)" = "0" ] &&
       [ "$(cat "$tmp/sw/$p.rc" 2>/dev/null)" = "0" ] &&
       [ "$td" = "$sw" ] && printf '%s' "$td" | grep -qF "$want"; then
        ok "$p: threaded and fallback builds print the same ($want)"
    else
        bad "$p: threaded and fallback builds differ"
        echo "      threaded:"; printf '%s\n' "$td" | sed 's/^/        |/'
        echo "      fallback:"; printf '%s\n' "$sw" | sed 's/^/        |/'
    fi
}
check_same main "745269870"
check_same reg "threaded dispatch: all cases agree"
check_same neg "3 3 3 2 1 1"

if command -v nm > /dev/null 2>&1 && [ -f "$tmp/td/main" ] && [ -f "$tmp/sw/main" ]; then
    if nm "$tmp/td/main" 2>/dev/null | grep -q '_ae_td_tbl' &&
       ! nm "$tmp/sw/main" 2>/dev/null | grep -q '_ae_td_tbl'; then
        ok "the fallback build really has no dispatch table"
    else
        bad "the threaded build lacks a table, or the fallback has one"
    fi
fi

# --- 4. --emit=lib deadline --------------------------------------------------
if "$AETHERC" --emit=lib "$SCRIPT_DIR/fixtures/lib_vm.ae" "$tmp/lib.c" > "$tmp/lib.log" 2>&1 &&
   grep -q 'aether_caps_deadline_tripped()) { __aether_abort_call(); goto _ae_td_exit_1; }' "$tmp/lib.c" &&
   grep -q '_ae_td_exit_1: ;' "$tmp/lib.c"; then
    ok "--emit=lib: each threaded dispatch checks the call deadline"
else
    bad "--emit=lib: the threaded dispatch skips the deadline check"
    sed 's/^/        /' "$tmp/lib.log" | head -10
fi
if (cd "$tmp" && "$AE" build --emit=lib "$SCRIPT_DIR/fixtures/lib_vm.ae" -o "$tmp/libvm" > "$tmp/libbuild.log" 2>&1); then
    ok "--emit=lib: a library with a threaded loop builds"
else
    bad "--emit=lib: the library did not build"
    sed 's/^/        /' "$tmp/libbuild.log" | head -10
fi

echo ""
echo "threaded dispatch: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
