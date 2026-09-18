#!/bin/sh
# Regression #1252: the interop lowering cast literal format strings to
# void*, which stripped the constant the C compiler's -Wformat check
# reads. A format/argument bug in a printf-family extern call compiled
# with no warning, even against libc's own attributed prototype. The
# lowering now emits string literals bare into ptr parameters, ae passes
# -Wformat, and the #line mapping points the warning at the .ae source.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

cat > "$TMPDIR/fmtbug.ae" <<'AEOF'
extern printf(fmt: ptr, ...) -> int

main() {
    printf("value is %s\n", 42)
    return 0
}
AEOF

# The build must SUCCEED (a warning, not an error) and the C compiler's
# format diagnostic must be visible and attributed to the .ae file.
if ! "$AE" build "$TMPDIR/fmtbug.ae" -o "$TMPDIR/fmtbug" >"$TMPDIR/out.log" 2>&1; then
    echo "  [FAIL] build errored; -Wformat should warn, not fail"
    head -10 "$TMPDIR/out.log"
    exit 1
fi

if ! grep -qi "format" "$TMPDIR/out.log"; then
    echo "  [FAIL] no format diagnostic surfaced; the void* cast is back (#1252)"
    head -10 "$TMPDIR/out.log"
    exit 1
fi

if ! grep -q "fmtbug.ae" "$TMPDIR/out.log"; then
    echo "  [FAIL] diagnostic not attributed to the .ae source"
    grep -i format "$TMPDIR/out.log" | head -3
    exit 1
fi

echo "  [PASS] wformat_extern_literal: format bug in extern call warns at the .ae line"
