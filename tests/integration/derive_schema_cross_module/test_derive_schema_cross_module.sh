#!/bin/sh
# @derive(schema) on a struct in an imported module (#2298).
#
# The derive pass runs after the module merge, so the table and `T_schema()`
# are synthesized in the consumer's program: the module's own functions call
# the accessor by its bare name, an importer calls it qualified when the
# module exports it, and the bare name reaches the same table. The field
# attributes survive the merge's clone of the struct.
#
# Acceptance: compiles, links, runs, prints "3", "0.5", "Health max 1000",
# "true".

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] derive_schema_cross_module: program errored (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' '|' | sed 's/|$//')
if [ "$got" != "3|0.5|Health max 1000|true" ]; then
    echo "  [FAIL] derive_schema_cross_module: expected '3|0.5|Health max 1000|true', got '$got'"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] derive_schema_cross_module: an imported struct's schema, from the module and the importer"
exit 0
