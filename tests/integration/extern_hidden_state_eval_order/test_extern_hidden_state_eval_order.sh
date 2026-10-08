#!/bin/sh
# #2524: an extern call whose effects are not declared has unknown effects.
#
# `pair(tick(), peek())`, where `tick` advances a C static that `peek`
# reads, emitted both calls inline as C arguments, and gcc evaluated
# `peek` first on Windows. An extern (and a function of the program that
# calls one) is opaque now: it is evaluated into a temporary ahead of any
# later operand that calls anything or reads memory through a pointer.
# Operands that read only plain locals and literals stay inline, beside a
# call or not.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"
AETHERC="$ROOT/build/aetherc"

if [ ! -x "$AE" ] && [ ! -x "$AE.exe" ]; then
    echo "  [SKIP] extern_hidden_state_eval_order: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
export AETHER_HOME="$ROOT"
fail=0

if ! "$AE" build "$SCRIPT_DIR/main.ae" -o "$tmp/main" --extra "$SCRIPT_DIR/shim.c" >"$tmp/build.log" 2>&1; then
    echo "  [FAIL] extern_hidden_state_eval_order: build failed"
    sed 's/^/        /' "$tmp/build.log" | head -15
    exit 1
fi

got="$("$tmp/main" 2>&1 | tr -d '\r')"
want="hidden 11 12 22
wrapped 33 34
plain 82 57 -4"
if [ "$got" != "$want" ]; then
    echo "  [FAIL] extern_hidden_state_eval_order: an extern's hidden state was read out of order"
    printf 'got:\n%s\nwant:\n%s\n' "$got" "$want" | sed 's/^/        /'
    fail=1
fi

# The emitted C: the hidden-state pairs hoist their first operand; the
# plain pairs do not, even beside a call.
if ! "$AETHERC" "$SCRIPT_DIR/main.ae" "$tmp/main.c" >/dev/null 2>&1; then
    echo "  [FAIL] extern_hidden_state_eval_order: main.ae did not compile to C"
    fail=1
else
    if ! grep -q '_eo[0-9]* = (tick())' "$tmp/main.c"; then
        echo "  [FAIL] extern_hidden_state_eval_order: tick() beside peek() was not evaluated ahead"
        fail=1
    fi
    if grep -q '_eo[0-9]* = (x' "$tmp/main.c"; then
        echo "  [FAIL] extern_hidden_state_eval_order: plain local arithmetic was hoisted"
        grep -n '_eo[0-9]* = (x' "$tmp/main.c" | head -3 | sed 's/^/        /'
        fail=1
    fi
    if ! grep -q 'pair((x + y), (x \* y))' "$tmp/main.c"; then
        echo "  [FAIL] extern_hidden_state_eval_order: plain local arithmetic is not emitted inline"
        grep -n 'pair(' "$tmp/main.c" | head -5 | sed 's/^/        /'
        fail=1
    fi
fi

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "  [PASS] extern_hidden_state_eval_order: an extern's hidden state is read in source order and plain operands stay inline"
