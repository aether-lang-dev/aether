#!/bin/sh
# Issue #243 sealed-scope follow-up: encapsulation hole closure.
#
# Asserts that user code calling a transitively-pulled-in namespace
# DIRECTLY (without an explicit import) is rejected at typecheck
# time. Round-1's BFS-merge fix accidentally leaked transitive
# namespaces; this commit gates user-side qualified-call resolution
# to namespaces the user explicitly imported.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

# `ae build` must FAIL — the build script wraps in cd to satisfy the
# auto-discovered lib/ resolution that finds lib_a/ and lib_b/.
cd "$SCRIPT_DIR"
if "$AE" build main.ae -o "$TMPDIR/sealed" >"$TMPDIR/out.log" 2>&1; then
    echo "  [FAIL] build unexpectedly succeeded — sealed namespaces not enforced"
    cat "$TMPDIR/out.log" | head -20
    exit 1
fi

# Verify the error is the expected shape (some flavor of "undefined"
# pointing at lib_b.shout). Accept either E0301 wording or a
# "namespace not visible" wording — implementations vary.
if grep -iE "undefined.*lib_b|lib_b.*not.*visible|lib_b.*not.*imported" \
        "$TMPDIR/out.log" >/dev/null 2>&1; then
    echo "  [PASS] sealed_namespaces: user code cannot call transitively-merged namespace directly"
    exit 0
fi

echo "  [FAIL] build failed but error message did not mention lib_b:"
cat "$TMPDIR/out.log" | head -20
exit 1
