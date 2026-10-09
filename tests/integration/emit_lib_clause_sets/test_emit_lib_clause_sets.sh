#!/bin/sh
# --emit=lib with exports written as clauses or bound to C symbols
# (#2665, #2666).
#
# The catalog (the JSON and the aether_lib_meta table), the header and the
# alias stubs read each clause of a set as an export of its own: a set was
# listed once per clause, its alias stub was emitted once per clause (on
# main: "redefinition of 'aether_sign'"), and a set whose first clause has a
# literal pattern (`fact(0)`) lost its alias while the catalog, gating on a
# later clause, still named `aether_fact`, which the library did not define.
# A set is now one export with its set's signature, listed once, and gets
# its alias when that signature is representable. The alias stub of a
# `@c_callback("sym")` function called the Aether name, which no C function
# carries, so any library exporting one failed to build (#2666); it calls
# the bound symbol.
#
# Fixture: clauses.ae exports `sign` (guards), `fact` (a literal first
# clause), `triple` (@c_callback("cl_triple")), `pick` (a clause set bound
# to "cl_pick"), an `export`-wrapped `add` and `plain`.
#
#   1. --emit=csrc: the catalog lists each export once, with its symbol, and
#      the header declares each alias once, with no alias skipped;
#   2. --emit=lib builds, and a C host finds every symbol the catalog names
#      in the library and gets the right answers from each export.
#
# Both builds write into a temporary directory (-o), removed on exit.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] emit_lib_clause_sets: $AE not built"
    exit 0
fi

LDL="-ldl"
case "$(uname -s 2>/dev/null)" in
    Darwin) LIB_EXT=".dylib" ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT) LIB_EXT=".dll"; LDL="" ;;
    *)      LIB_EXT=".so" ;;
esac

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
cd "$SCRIPT_DIR" || exit 1

fail=0

# --- 1. the catalog and the header ------------------------------------------
if ! AETHER_HOME="" "$AE" build --emit=csrc clauses.ae -o "$TMPDIR/clauses" >"$TMPDIR/csrc.log" 2>&1; then
    echo "  [FAIL] ae build --emit=csrc failed:"
    sed 's/^/        /' "$TMPDIR/csrc.log" | head -15
    exit 1
fi
JSON="$TMPDIR/clauses.catalog.json"
HDR="$TMPDIR/clauses.h"
names="$(tr -d '\r' < "$JSON" | grep -o '"aether_name": "[^"]*"' | sed 's/.*: "//; s/"$//' | sort | tr '\n' ' ')"
want_names="add fact pick plain sign triple "
if [ "$names" != "$want_names" ]; then
    echo "  [FAIL] the catalog lists '$names', want each export once: '$want_names'"
    fail=1
fi
syms="$(tr -d '\r' < "$JSON" | grep -o '"c_symbol": "[^"]*"' | sed 's/.*: "//; s/"$//' | sort | tr '\n' ' ')"
want_syms="aether_add aether_fact aether_plain aether_sign cl_pick cl_triple "
if [ "$syms" != "$want_syms" ]; then
    echo "  [FAIL] the catalog's symbols are '$syms', want '$want_syms'"
    fail=1
fi
if ! tr -d '\r' < "$JSON" | grep -q '"aether_name": "fact", "c_symbol": "aether_fact", "signature": "(int) -> int"'; then
    echo "  [FAIL] fact is not listed with its set's signature (int) -> int:"
    grep '"fact"' "$JSON" | sed 's/^/        /'
    fail=1
fi
for a in aether_sign aether_fact aether_triple aether_pick aether_add aether_plain; do
    n="$(grep -c "^int32_t $a(int32_t" "$HDR")"
    if [ "$n" != "1" ]; then
        echo "  [FAIL] the header declares $a $n times, want 1"
        fail=1
    fi
done
if grep -q "skipping alias stub" "$TMPDIR/csrc.log"; then
    echo "  [FAIL] an alias stub was skipped:"
    grep -A1 "skipping alias stub" "$TMPDIR/csrc.log" | sed 's/^/        /'
    fail=1
fi
[ "$fail" -eq 0 ] && echo "  [PASS] the catalog and the header list each export once, with its set's signature"

# --- 2. the library, from a C host -------------------------------------------
if ! AETHER_HOME="" "$AE" build --emit=lib clauses.ae -o "$TMPDIR/libclauses" >"$TMPDIR/lib.log" 2>&1; then
    echo "  [FAIL] ae build --emit=lib failed:"
    sed 's/^/        /' "$TMPDIR/lib.log" | head -15
    exit 1
fi
LIB_PATH=""
for candidate in "$TMPDIR/libclauses" "$TMPDIR/libclauses$LIB_EXT"; do
    [ -f "$candidate" ] && { LIB_PATH="$candidate"; break; }
done
if [ -z "$LIB_PATH" ]; then
    echo "  [FAIL] ae build --emit=lib produced no library"
    ls -la "$TMPDIR"
    exit 1
fi
# shellcheck disable=SC2086
if ! gcc "$SCRIPT_DIR/host.c" $LDL -o "$TMPDIR/host" 2>"$TMPDIR/gcc.log"; then
    echo "  [FAIL] gcc could not compile host.c:"
    sed 's/^/        /' "$TMPDIR/gcc.log" | head -10
    exit 1
fi
# shellcheck disable=SC2086
if "$TMPDIR/host" "$LIB_PATH" $syms >"$TMPDIR/host.out" 2>&1 && grep -q "^OK" "$TMPDIR/host.out"; then
    echo "  [PASS] the library exports every symbol its catalog names, and each answers"
else
    echo "  [FAIL] the host reported:"
    sed 's/^/        /' "$TMPDIR/host.out" | head -15
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "PASS: emit_lib_clause_sets (#2665, #2666)"
    exit 0
fi
echo "FAIL: emit_lib_clause_sets (#2665, #2666)"
exit 1
