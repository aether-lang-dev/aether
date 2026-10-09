#!/bin/sh
# Regression for #2585 and #2589: a call statement with a later call
# argument has its earlier operands evaluated into temps first (the `_eo`
# evaluation-order wrapper). The statement's "value discarded" flag belonged
# to the call, but the first operand call took it: a hoisted call that
# drains a string was cast to void, so `__auto_type _eo0` was declared void
# and the C did not compile (#2589), and the statement's own drain, finding
# the flag cleared, left its result as an unused value (#2585, clang
# -Wunused-value, fatal in a warnings-as-errors build).
#
# Builds the generated C with the host C compiler (catches #2589), with
# clang -Werror=unused-value where clang is installed (catches #2585), and
# runs the program.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
EXE="${EXE_EXT:-}"
if [ -z "$EXE" ] && [ ! -x "$ROOT/build/aetherc" ] && [ -x "$ROOT/build/aetherc.exe" ]; then
    EXE=".exe"
fi
AETHERC="$ROOT/build/aetherc$EXE"
AE="$ROOT/build/ae$EXE"
if [ ! -x "$AETHERC" ] || [ ! -x "$AE" ]; then
    echo "  [SKIP] order_hoist_call_statement: compiler not built"
    exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

if ! "$AETHERC" "$SCRIPT_DIR/probe.ae" "$TMPDIR/out.c" >"$TMPDIR/cc.log" 2>&1; then
    echo "  [FAIL] order_hoist_call_statement: aetherc"
    head -20 "$TMPDIR/cc.log"
    exit 1
fi

INC="-I$ROOT/runtime -I$ROOT/runtime/actors -I$ROOT/std -I$ROOT/std/io -I$ROOT/std/collections"
CC_BIN="${CC:-cc}"
command -v "$CC_BIN" >/dev/null 2>&1 || CC_BIN=gcc
if ! "$CC_BIN" $INC -c "$TMPDIR/out.c" -o "$TMPDIR/out.o" 2>"$TMPDIR/c.log"; then
    echo "  [FAIL] order_hoist_call_statement: the generated C does not compile (#2589)"
    grep -m5 "error" "$TMPDIR/c.log" | sed 's/^/        /'
    exit 1
fi
if command -v clang >/dev/null 2>&1; then
    if ! clang -Werror=unused-value $INC -c "$TMPDIR/out.c" -o "$TMPDIR/out_clang.o" 2>"$TMPDIR/clang.log"; then
        echo "  [FAIL] order_hoist_call_statement: clang -Werror=unused-value on the generated C (#2585)"
        grep -m5 "error" "$TMPDIR/clang.log" | sed 's/^/        /'
        exit 1
    fi
fi

if ! ( cd "$TMPDIR" && "$AE" build "$SCRIPT_DIR/probe.ae" -o "$TMPDIR/probe" ) >"$TMPDIR/build.log" 2>&1; then
    echo "  [FAIL] order_hoist_call_statement: ae build"
    head -20 "$TMPDIR/build.log" | sed 's/^/        /'
    exit 1
fi
got="$("$TMPDIR/probe$EXE" 2>&1)"
want="1 clip walk_01
state 5 share 0.7"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] order_hoist_call_statement: expected:"
    echo "$want" | sed 's/^/        /'
    echo "      got:"
    echo "$got" | sed 's/^/        /'
    exit 1
fi
echo "  [PASS] order_hoist_call_statement: a wrapped call statement compiles, warns about nothing, and runs"
exit 0
