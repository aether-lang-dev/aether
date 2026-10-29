#!/bin/sh
# When both the embedded checker and preload grants are active, an operation
# must satisfy both policies. Neither policy may replace the other.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LIB="$ROOT/build/libaether_sandbox.so"

case "$(uname -s 2>/dev/null)" in
    Linux|FreeBSD) ;;
    *)
        echo "  [SKIP] sandbox_policy_intersection: preload is Linux and FreeBSD only"
        exit 0 ;;
esac
if [ ! -f "$LIB" ]; then
    echo "  [SKIP] sandbox_policy_intersection: preload library not built"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
if ! ${CC:-cc} -std=c11 -Wall -Wextra -Werror -rdynamic \
        "$SCRIPT_DIR/harness.c" -o "$TMP/harness"; then
    echo "  [FAIL] sandbox_policy_intersection: harness did not compile"
    exit 1
fi

printf 'intersection sentinel\n' > "$TMP/data.txt"
printf 'exec:/not-used\n' > "$TMP/deny-read.grants"
printf 'fs_read:%s\n' "$TMP/data.txt" > "$TMP/allow-read.grants"
fail=0

run_case() {
    label="$1"
    checker="$2"
    grants="$3"
    expected="$4"
    if LD_PRELOAD="$LIB" AETHER_SANDBOX_GRANTS="$grants" \
            AETHER_SANDBOX_LOG=none "$TMP/harness" "$checker" "$TMP/data.txt" \
            >"$TMP/out" 2>"$TMP/err"; then
        got=allow
    else
        got=deny
    fi
    if [ "$got" = "$expected" ]; then
        echo "  [PASS] $label"
    else
        echo "  [FAIL] $label: got $got, expected $expected"
        fail=1
    fi
}

run_case "preload denial survives an allowing embedded checker" \
    allow "$TMP/deny-read.grants" deny
run_case "embedded denial survives an allowing preload policy" \
    deny "$TMP/allow-read.grants" deny
run_case "both policies allowing permits the read" \
    allow "$TMP/allow-read.grants" allow

echo ""
echo "sandbox_policy_intersection: $([ "$fail" -eq 0 ] && echo PASS || echo FAIL)"
[ "$fail" -eq 0 ]
