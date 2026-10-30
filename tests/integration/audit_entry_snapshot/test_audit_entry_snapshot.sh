#!/bin/sh
# Reading one audit entry must not combine fields from different ring slots
# when another thread overwrites the oldest entry between accessor calls.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    Linux|FreeBSD|Darwin) ;;
    *)
        echo "  [SKIP] audit_entry_snapshot: POSIX thread harness"
        exit 0 ;;
esac
if [ ! -f "$ROOT/build/libaether.a" ]; then
    echo "  [SKIP] audit_entry_snapshot: build/libaether.a not built"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
if ! ${CC:-cc} -std=c11 -Wall -Wextra -Werror \
        -I"$ROOT/runtime/sandbox" -o "$TMP/harness" "$SCRIPT_DIR/harness.c" \
        "$ROOT/build/libaether.a" -lpthread -lm 2>"$TMP/cc.err"; then
    echo "  [FAIL] audit_entry_snapshot: harness did not compile"
    sed 's/^/          /' "$TMP/cc.err" | head -10
    exit 1
fi

out="$($TMP/harness 2>&1)"
rc=$?
if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q \
        '^PASS: audit entry fields come from one snapshot$'; then
    echo "  [PASS] audit entry fields come from one snapshot"
    echo "PASS: audit_entry_snapshot"
    exit 0
fi

echo "  [FAIL] audit entry fields came from different snapshots"
printf '%s\n' "$out" | sed 's/^/          /' | head -10
echo "FAIL: audit_entry_snapshot"
exit 1
