#!/bin/sh
# #2614: a module reaches the modules IT imports, not the ones some other
# module of the program imports.
#
# Inside a module's merged functions a qualified `ns.name` resolved against
# every namespace anywhere in the build, so `top` could call `low.low_value()`
# while only `mid` imported `low`. `ae build` of any program accepted it and
# `ae check lib/top/module.ae` rejected it (E0301): the module worked by the
# accident of what else was linked in, and would break, pointing at code
# nobody touched, the day `mid` stopped importing `low` (aephysics'
# `mesh.collide_mover_and_hull` called `hull.hull_proxy` this way).
#
# Fixture: lib/low <- lib/mid (imports low) <- lib/top (imports only mid,
# calls low). lib/topconst reads `low.LOW_BASE` the same way. lib/topok
# imports both.
#
#   1. building a program that imports `top` fails with E0301 at the call in
#      lib/top, and the help names the missing `import low`;
#   2. `ae check lib/top/module.ae` rejects the same call: one rule;
#   3. the constant form is rejected too, naming the same import;
#   4. control: with `import low` written, the program builds and prints 15.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

fail=0
cd "$SCRIPT_DIR"

# --- 1. ae build rejects the call and names the import ---------------------
if "$AE" build main.ae -o "$TMP/main" >"$TMP/build.log" 2>&1; then
    echo "  [FAIL] main.ae built: lib/top calls low without importing it"
    fail=1
elif ! grep -q "error\[E0301\]: Undefined function 'low.low_value'" "$TMP/build.log" ||
     ! grep -q "top/module.ae:9:" "$TMP/build.log"; then
    echo "  [FAIL] main.ae failed, but not with E0301 at lib/top/module.ae:9:"
    sed 's/^/        /' "$TMP/build.log" | head -12
    fail=1
elif ! grep -q "add \`import low\`" "$TMP/build.log"; then
    echo "  [FAIL] the E0301 does not name the missing import:"
    sed 's/^/        /' "$TMP/build.log" | head -12
    fail=1
else
    echo "  [PASS] ae build rejects a call into a module the caller does not import"
fi

# --- 2. ae check of the module says the same ------------------------------
if "$AE" check lib/top/module.ae >"$TMP/check.log" 2>&1; then
    echo "  [FAIL] ae check lib/top/module.ae passed"
    fail=1
elif ! grep -q "error\[E0301\]: Undefined function 'low.low_value'" "$TMP/check.log"; then
    echo "  [FAIL] ae check lib/top/module.ae failed differently:"
    sed 's/^/        /' "$TMP/check.log" | head -12
    fail=1
else
    echo "  [PASS] ae check of the module rejects the same call"
fi

# --- 3. a qualified constant is held to the same rule ---------------------
if "$AE" build main_const.ae -o "$TMP/const" >"$TMP/const.log" 2>&1; then
    echo "  [FAIL] main_const.ae built: lib/topconst reads low.LOW_BASE without importing low"
    fail=1
elif ! grep -q "topconst/module.ae:7:" "$TMP/const.log" ||
     ! grep -q "add \`import low\`" "$TMP/const.log"; then
    echo "  [FAIL] main_const.ae failed, but not at lib/topconst naming the import:"
    sed 's/^/        /' "$TMP/const.log" | head -12
    fail=1
else
    echo "  [PASS] a qualified constant of an unimported module is rejected too"
fi

# --- 4. control: importing low makes the same body build -------------------
out="$("$AE" run main_ok.ae 2>&1)"
if [ "$(printf '%s\n' "$out" | tail -1)" != "15" ]; then
    echo "  [FAIL] main_ok.ae (lib/topok imports low) did not print 15:"
    printf '%s\n' "$out" | sed 's/^/        /' | head -8
    fail=1
else
    echo "  [PASS] the same call builds once the module imports low itself"
fi

if [ "$fail" -eq 0 ]; then
    echo "PASS: module_import_not_transitive (#2614)"
    exit 0
fi
echo "FAIL: module_import_not_transitive (#2614)"
exit 1
