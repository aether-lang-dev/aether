#!/bin/sh
# #2613: every function of every module in a build is type-checked, whether
# or not the program calls it.
#
# The prune that keeps uncalled library functions out of the emitted C ran
# BEFORE the typechecker, so a function of an imported module that nothing
# called was never checked at all. A library could carry a type error for
# months and build clean in every consumer until one of them first called
# it (aephysics' `dynamics.body_apply_force` added a Vec3 to a Vec3f this
# way), while the same function, uncalled, in the main file failed the build.
#
# Fixture: lib/shapes exports `area` (called) and `broken_scale` (never
# called, stores a struct in an int field). lib/tidy exports `used` (called)
# and `unused` (never called, correct).
#
#   1. a program calling only `shapes.area` fails, at the line in the module;
#   2. so does one importing it selectively, `import shapes (area)`: the
#      selection names what the file may write bare, not what gets checked;
#   3. `ae check` of the program agrees with `ae build`;
#   4. control: a correct uncalled function builds, runs, and is still left
#      out of the emitted C (the prune now runs after checking, not never);
#   5. what checking records inside a function the prune then drops (a
#      `sandbox.enforce` trusted call) is dropped with it.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

fail=0
cd "$SCRIPT_DIR"

# The diagnostic for broken_scale: its file and line, and the mismatch.
names_broken_scale() {
    grep -q "shapes/module.ae:15:" "$1" && grep -q "Type mismatch in assignment" "$1"
}

# --- 1. the uncalled function's error fails the build ----------------------
if "$AE" build main.ae -o "$TMP/main" >"$TMP/build.log" 2>&1; then
    echo "  [FAIL] main.ae built: shapes.broken_scale was never type-checked"
    fail=1
elif ! names_broken_scale "$TMP/build.log"; then
    echo "  [FAIL] main.ae failed, but not at shapes/module.ae:15 (broken_scale):"
    sed 's/^/        /' "$TMP/build.log" | head -12
    fail=1
else
    echo "  [PASS] an uncalled module function's type error fails the build"
fi

# --- 2. a selective import checks the rest of the module too ---------------
if "$AE" build main_selective.ae -o "$TMP/sel" >"$TMP/sel.log" 2>&1; then
    echo "  [FAIL] main_selective.ae built: the unselected broken_scale was never checked"
    fail=1
elif ! names_broken_scale "$TMP/sel.log"; then
    echo "  [FAIL] main_selective.ae failed, but not at broken_scale:"
    sed 's/^/        /' "$TMP/sel.log" | head -12
    fail=1
else
    echo "  [PASS] a selective import still checks every function of the module"
fi

# --- 3. ae check of the program agrees --------------------------------------
if "$AE" check main.ae >"$TMP/check.log" 2>&1; then
    echo "  [FAIL] ae check main.ae passed: the uncalled broken_scale was never checked"
    fail=1
elif ! names_broken_scale "$TMP/check.log"; then
    echo "  [FAIL] ae check main.ae failed, but not at broken_scale:"
    sed 's/^/        /' "$TMP/check.log" | head -12
    fail=1
else
    echo "  [PASS] ae check of the program reports the same error"
fi

# --- 4. control: checked, then still dropped from the C ---------------------
if ! "$AE" build --emit=csrc main_tidy.ae -o "$TMP/tidy" >"$TMP/tidy.log" 2>&1; then
    echo "  [FAIL] main_tidy.ae (correct uncalled function) did not build:"
    sed 's/^/        /' "$TMP/tidy.log" | head -12
    fail=1
elif ! grep -q "tidy_used" "$TMP/tidy.c"; then
    echo "  [FAIL] the called tidy.used is missing from the emitted C"
    fail=1
elif grep -q "tidy_unused" "$TMP/tidy.c"; then
    echo "  [FAIL] the uncalled tidy.unused reached the emitted C: the prune no longer drops it"
    grep -n "tidy_unused" "$TMP/tidy.c" | head -3 | sed 's/^/        /'
    fail=1
else
    out="$("$AE" run main_tidy.ae 2>&1)"
    if [ "$(printf '%s\n' "$out" | tail -1)" != "1" ]; then
        echo "  [FAIL] main_tidy.ae did not print 1:"
        printf '%s\n' "$out" | sed 's/^/        /' | head -8
        fail=1
    else
        echo "  [PASS] a correct uncalled function is checked, then left out of the C"
    fi
fi

# --- 5. what checking recorded inside a dropped function goes with it ------
# lib/guard's uncalled `unused_guarded` holds a `sandbox.enforce(w,
# hidden_peek)` whose target nothing calls either. Checking it records the
# trusted call; the prune then frees both functions, and codegen must not
# emit a wrapper for that call (it named a function no longer there).
out="$("$AE" run main_guard.ae 2>&1)"
if ! printf '%s\n' "$out" | grep -q "^peek: s3cret$" ||
   ! printf '%s\n' "$out" | grep -q "^run: 1$"; then
    echo "  [FAIL] main_guard.ae (a trusted call inside an uncalled function) did not run:"
    printf '%s\n' "$out" | sed 's/^/        /' | head -12
    fail=1
else
    echo "  [PASS] a trusted call inside a dropped function is dropped with it"
fi

if [ "$fail" -eq 0 ]; then
    echo "PASS: uncalled_module_fn_checked (#2613)"
    exit 0
fi
echo "FAIL: uncalled_module_fn_checked (#2613)"
exit 1
