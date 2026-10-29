#!/bin/sh
# Grant transport accepts an empty deny-all policy, while rejecting malformed
# or unrepresentable lists before starting a child.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    Linux|FreeBSD) ;;
    *)
        echo "  [SKIP] sandbox_grant_transport: spawn_sandboxed is Linux and FreeBSD only"
        exit 0 ;;
esac
if [ ! -f "$ROOT/build/libaether.a" ] ||
   [ ! -f "$ROOT/build/libaether_sandbox.so" ]; then
    echo "  [SKIP] sandbox_grant_transport: sandbox runtime not built"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT

if ! ${CC:-cc} -std=c11 -Wall -Wextra -Werror \
        -o "$TMP/harness" "$SCRIPT_DIR/harness.c" \
        "$ROOT/build/libaether.a" -lpthread -lm 2>"$TMP/cc.err"; then
    if ! ${CC:-cc} -std=c11 -Wall -Wextra -Werror \
            -o "$TMP/harness" "$SCRIPT_DIR/harness.c" \
            "$ROOT/build/libaether.a" \
            -lpthread -ldl -lm -lrt -lz -lssl -lcrypto 2>"$TMP/cc.err"; then
        echo "  [FAIL] sandbox_grant_transport: harness did not link"
        sed 's/^/          /' "$TMP/cc.err" | head -8
        exit 1
    fi
fi

out="$(AETHER_SANDBOX_PRELOAD="$ROOT/build/libaether_sandbox.so" \
       AETHER_SANDBOX_LOG=none "$TMP/harness" 2>&1 </dev/null)"
rc=$?
if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q \
        '^PASS: sandbox grant transport validates complete input$'; then
    echo "  [PASS] sandbox grant transport accepts deny-all and rejects invalid input"
    echo "PASS: sandbox_grant_transport"
    exit 0
fi

echo "  [FAIL] sandbox grant transport accepted invalid input or rejected deny-all"
printf '%s\n' "$out" | sed 's/^/          /' | head -30
echo "FAIL: sandbox_grant_transport"
exit 1
