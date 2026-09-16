#!/bin/sh
# Regression: a whole-program build with MORE THAN 64 imported namespaces must
# still resolve every qualified call. The typechecker tracked imported
# namespaces in a fixed `char* imported_namespaces[64]`; a merged unit whose
# transitive import graph registered more than 64 namespaces silently dropped
# every one past the 64th, so a qualified call into a dropped namespace — even a
# COMPILER-GENERATED cleanup like `sha1.free_ctx` for a Sha1Ctx reached through
# the std.http/std.cryptography graph — failed with a spurious
# `E0301: Undefined function`. It presented as a graph-SIZE threshold: "add one
# more module and the build breaks." (selaenium, ae 0.677;
# asks/aether-0677-sha1-free-ctx-wholeprogram.md.)
#
# prog.ae imports 70 filler namespaces plus a `zlate` module registered after
# the first 64, and calls `zlate.shout` — which the old cap dropped. A plain
# `ae run` exercises the same >64-namespace type-check path (the ask hit it via
# --emit=lib, but the resolution path is identical and a plain build stays
# portable across Windows/macOS/Linux). The program must build, run, and print
# its own PASS.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if out="$(cd "$SCRIPT_DIR" && "$AE" run prog.ae 2>&1)" \
   && printf '%s' "$out" | grep -q "PASS many_namespaces_qualified_call"; then
    echo "  [PASS] >64-namespace merge resolves the late qualified call"
    echo "PASS: many_namespaces_qualified_call"
    exit 0
fi

echo "  [FAIL] >64-namespace merge did not build/run (the namespace-cap regression)"
printf '%s\n' "$out" | grep -iE "E0301|Undefined function|zlate|FAIL" | sed 's/^/          /' | head -6
echo "FAIL: many_namespaces_qualified_call"
exit 1
