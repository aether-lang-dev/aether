#!/bin/sh
# A sandbox grant must be honoured inside a spec.describe / spec.it block (#1704).
#
# The permission stack and the builder context stack were the same array, so
# entering any trailing block pushed that builder's config object onto the
# stack the permission checker walks. `_aether_perms_allow` then read a
# builder config as a permission list, found no entries, and denied
# everything. The same grant that worked in a plain function was refused
# inside a describe block, which silently turns a sandboxed test suite into
# one that tests nothing but denials.
#
# A grant for env:HOME must be allowed in all three positions.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

if ! "$AE" build "$SCRIPT_DIR/prog.ae" -o "$TMP/prog" > "$TMP/build.log" 2>&1; then
    echo "  [FAIL] sandbox_in_spec_block: build failed"
    sed 's/^/        /' "$TMP/build.log" | head -8
    exit 1
fi

"$TMP/prog" > "$TMP/out.txt" 2>&1 || true

fail=0
for label in outside in-describe in-it; do
    line="$(grep "^${label} " "$TMP/out.txt" || true)"
    case "$line" in
        *"allowed=1") ;;
        *) echo "  [FAIL] sandbox_in_spec_block: ${label}: expected allowed=1, got '${line}'"; fail=1 ;;
    esac
done
[ "$fail" -eq 0 ] || exit 1

echo "  [PASS] sandbox_in_spec_block: a grant holds inside describe and it, not just in a plain function"
