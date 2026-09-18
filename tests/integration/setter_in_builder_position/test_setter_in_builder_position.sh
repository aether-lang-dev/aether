#!/bin/sh
# Regression: a block SETTER called as a top-level node BUILDER must be a
# compile error, not a silent no-op.
#
# In a builder DSL, a plain `_ctx`-first function whose body only records
# config (e.g. `rspec(_ctx: ptr) { map_put(_ctx, ...) }`) is meant to be
# CALLED INSIDE a builder's trailing block: `mod.bundle() { rspec() }`.
# Written the old way — as a top-level node with its OWN trailing block,
# `mod.rspec() { ... }` — it compiles clean but runs nothing (the setter
# takes no closure, so the block is a DSL container that is never entered).
# The typechecker now rejects this and names the fix.
#
# The check is deliberately NARROW to stay false-positive-free. It fires
# ONLY when the callee is a plain `_ctx`-first function, the call carries
# its own trailing block, the callee's module also defines a `builder`,
# AND the callee yields no value. A widget-style DSL container
# (`panel(_ctx, title) { button() }`) in a module with NO builders is
# structurally identical but must NOT be flagged — and neither is one in a
# module WITH builders when it returns the handle its block runs inside
# (aether-ui's `ui` module: `builder window` beside `vstack(_ctx, spacing)`,
# which rejected every app under the first cut of this rule).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

fail=0

# A case that MUST be rejected: aetherc must report an error whose text
# identifies the setter as a "block setter" and suggests the builder-block
# fix. aetherc exits non-zero on a type error, but we inspect the log so
# the assertion is on the message, not just the code.
expect_error() {
    src="$1"
    label="$2"
    tmpdir="$(mktemp -d)"
    log="$tmpdir/cc.log"

    "$AETHERC" "$src" "$tmpdir/out.c" >"$log" 2>&1

    if ! grep -q '^error' "$log"; then
        echo "  [FAIL] $label: aetherc reported no error for a setter used as a node builder"
        sed 's/^/          /' "$log" | head -8
        fail=1
        rm -rf "$tmpdir"
        return
    fi
    if ! grep -qi "block setter" "$log"; then
        echo "  [FAIL] $label: error doesn't identify the callee as a 'block setter'"
        sed 's/^/          /' "$log" | head -8
        fail=1
        rm -rf "$tmpdir"
        return
    fi
    if ! grep -qi "builder's block" "$log"; then
        echo "  [FAIL] $label: error doesn't suggest calling it inside a builder's block"
        sed 's/^/          /' "$log" | head -8
        fail=1
        rm -rf "$tmpdir"
        return
    fi

    echo "  [PASS] $label"
    rm -rf "$tmpdir"
}

# A case that MUST compile cleanly: no error line at all, and aetherc
# exits 0.
expect_ok() {
    src="$1"
    label="$2"
    tmpdir="$(mktemp -d)"
    log="$tmpdir/cc.log"

    "$AETHERC" "$src" "$tmpdir/out.c" >"$log" 2>&1
    rc=$?

    if [ "$rc" -ne 0 ] || grep -q '^error' "$log"; then
        echo "  [FAIL] $label: aetherc newly errored on legitimate code (false positive)"
        sed 's/^/          /' "$log" | head -8
        fail=1
        rm -rf "$tmpdir"
        return
    fi

    echo "  [PASS] $label"
    rm -rf "$tmpdir"
}

expect_error "$SCRIPT_DIR/misuse.ae"  "MISUSE: setter called as top-level node builder must error"
expect_ok    "$SCRIPT_DIR/legit_a.ae" "LEGIT A: same setter called inside the builder's block compiles"
expect_ok    "$SCRIPT_DIR/legit_b.ae" "LEGIT B: widget-style DSL container (no builders) not flagged"
expect_ok    "$SCRIPT_DIR/legit_c.ae" "LEGIT C: handle-returning container in a module WITH a builder not flagged"
expect_error "$SCRIPT_DIR/misuse_b.ae" "MISUSE B: void setter of that same widget module still errors"

exit $fail
