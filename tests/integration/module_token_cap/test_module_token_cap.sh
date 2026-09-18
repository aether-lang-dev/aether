#!/bin/sh
# Regression: a module larger than the old MAX_MODULE_TOKENS (20000) must
# import fully. module_parse_file used to gate imported modules on that cap
# and truncate the token stream mid-file, silently dropping the module's tail
# declarations. The cap was raised to 100000 and has since been removed
# altogether (#2059; tests/integration/source_size_unbounded drives every
# former cap past its limit) — this keeps the original shape as a guard.
#
# bigmod/module.ae is ~2200 generated functions (>20k tokens). uses_bigmod.ae
# calls the FIRST, MIDDLE, and LAST of them — so a truncated import leaves the
# tail fn (bigmod.add_2199) undefined and type-checking fails. The whole
# module parses and the program prints total=3302.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] module_token_cap: $AE not built"
    exit 0
fi

OUT="$(AETHER_HOME="$ROOT" "$AE" run "$SCRIPT_DIR/uses_bigmod.ae" 2>&1)"
RC=$?
if [ $RC -ne 0 ]; then
    echo "  [FAIL] module_token_cap: large imported module did not compile/run"
    echo "$OUT" | head -8
    exit 1
fi
if echo "$OUT" | grep -Fxq "total=3302"; then
    echo "  [PASS] module_token_cap: >20k-token imported module parses fully"
else
    echo "  [FAIL] module_token_cap: unexpected output"
    echo "$OUT" | head -8
    exit 1
fi
