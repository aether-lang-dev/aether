#!/bin/sh
# AETHER_SANDBOX_VERBOSE is a legacy boolean flag. Only the value 1 may
# override AETHER_SANDBOX_LOG; the zero value set by spawn_sandboxed is quiet.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LIB="$ROOT/build/libaether_sandbox.so"

case "$(uname -s 2>/dev/null)" in
    Linux|FreeBSD) ;;
    *)
        echo "  [SKIP] sandbox_verbose_flag: preload is Linux and FreeBSD only"
        exit 0 ;;
esac
if [ ! -f "$LIB" ]; then
    echo "  [SKIP] sandbox_verbose_flag: preload library not built"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
: > "$TMP/empty.grants"
fail=0

LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/empty.grants" \
    AETHER_SANDBOX_LOG=none AETHER_SANDBOX_VERBOSE=0 \
    /bin/true 2>"$TMP/zero.err"
if grep -q '^AETHER_SANDBOX:' "$TMP/zero.err"; then
    echo "  [FAIL] AETHER_SANDBOX_VERBOSE=0 enabled stderr logging"
    fail=1
else
    echo "  [PASS] AETHER_SANDBOX_VERBOSE=0 remains quiet"
fi

LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$TMP/empty.grants" \
    AETHER_SANDBOX_LOG=none AETHER_SANDBOX_VERBOSE=1 \
    /bin/true 2>"$TMP/one.err"
if grep -q '^AETHER_SANDBOX: active with 0 grants$' "$TMP/one.err"; then
    echo "  [PASS] AETHER_SANDBOX_VERBOSE=1 enables stderr logging"
else
    echo "  [FAIL] AETHER_SANDBOX_VERBOSE=1 did not enable stderr logging"
    fail=1
fi

echo ""
echo "sandbox_verbose_flag: $([ "$fail" -eq 0 ] && echo PASS || echo FAIL)"
[ "$fail" -eq 0 ]
