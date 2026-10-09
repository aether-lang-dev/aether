#!/bin/sh
# #2632: a glob import does not bind a name the importing file defines
# itself. The file's own function, builder, constant or extern wins, as a
# local item shadows a glob import in Rust.
#
# The typechecker registered a bare alias for every name of a glob-imported
# module, the file's own included, and rewrote the file's own calls to the
# module's: a program with `import std.string (*)` and its own
# `bytes(a, b, c)` failed to build with E0200 "Function 'string.bytes'
# expects 1 argument(s), got 3", and `ae check std/number/module.ae` failed
# the same way. Inside a merged module the merge renames the module's own
# functions first, but its own externs keep their bare name and were
# rewritten to the glob's.
#
# Fixture: lib/mathy exports `abs` (returns 99) and `twice`. lib/numfmt
# glob-imports std.string and mathy and defines its own `bytes` and
# `extern abs` (libc's).
#
#   1. a program's own `bytes` wins over std.string's, and the glob still
#      binds the names it does not define (`length`), in a build and in
#      `ae check`;
#   2. a program's own `extern abs` wins over mathy's;
#   3. a module checked on its own (lib/numfmt) passes, and a program
#      importing it runs its own `bytes` and libc's `abs`;
#   4. `ae check std/number/module.ae` passes;
#   5. a selective import of a name the file defines stays the E1000 clash.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

fail=0
cd "$SCRIPT_DIR"

# runs <file> <expected last line> <label>
runs() {
    out="$("$AE" run "$1" 2>&1)"
    if [ "$(printf '%s\n' "$out" | tail -1)" != "$2" ]; then
        echo "  [FAIL] $3: $1 did not print '$2':"
        printf '%s\n' "$out" | sed 's/^/        /' | head -10
        fail=1
        return 1
    fi
    return 0
}

# --- 1. the program's own function wins -------------------------------------
if runs prog.ae "6 3" "own function vs glob"; then
    if "$AE" check prog.ae >"$TMP/prog_check.log" 2>&1; then
        echo "  [PASS] a program's own function shadows the glob's, in build and check"
    else
        echo "  [FAIL] ae check prog.ae rejects the program's own bytes:"
        sed 's/^/        /' "$TMP/prog_check.log" | head -10
        fail=1
    fi
fi

# --- 2. the program's own extern wins ---------------------------------------
if runs prog_extern.ae "5 8" "own extern vs glob"; then
    echo "  [PASS] a program's own extern shadows the glob's"
fi

# --- 3. a module that glob-imports, checked alone and merged ----------------
if ! "$AE" check lib/numfmt/module.ae >"$TMP/mod_check.log" 2>&1; then
    echo "  [FAIL] ae check lib/numfmt/module.ae rejects the module's own names:"
    sed 's/^/        /' "$TMP/mod_check.log" | head -10
    fail=1
elif runs main_mod.ae "7 B -14" "module's own names vs its globs"; then
    echo "  [PASS] a module's own function and extern shadow its globs, checked alone and merged"
fi

# --- 4. std.number checks on its own -----------------------------------------
if "$AE" check "$ROOT/std/number/module.ae" >"$TMP/number.log" 2>&1; then
    echo "  [PASS] ae check std/number/module.ae passes"
else
    echo "  [FAIL] ae check std/number/module.ae fails:"
    sed 's/^/        /' "$TMP/number.log" | head -10
    fail=1
fi

# --- 5. a selective import of the same name is still refused ----------------
if "$AE" build prog_selective.ae -o "$TMP/sel" >"$TMP/sel.log" 2>&1; then
    echo "  [FAIL] prog_selective.ae built: a selected name clashing with a local is no longer E1000"
    fail=1
elif ! grep -q "E1000" "$TMP/sel.log"; then
    echo "  [FAIL] prog_selective.ae failed, but not with E1000:"
    sed 's/^/        /' "$TMP/sel.log" | head -10
    fail=1
else
    echo "  [PASS] a selective import of a name the file defines stays E1000"
fi

if [ "$fail" -eq 0 ]; then
    echo "PASS: glob_import_own_name (#2632)"
    exit 0
fi
echo "FAIL: glob_import_own_name (#2632)"
exit 1
