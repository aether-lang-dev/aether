#!/bin/sh
# An empty grant list starts the child under an active sandbox with nothing
# allowed: its file read, TCP connect and exec are all refused. The same child
# with matching grants is allowed all three, so the test shows confinement
# rather than a broken child.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    Linux|FreeBSD) ;;
    *)
        echo "  [SKIP] sandbox_empty_grants: spawn_sandboxed is Linux and FreeBSD only"
        exit 0 ;;
esac
if [ ! -f "$ROOT/build/libaether.a" ] ||
   [ ! -f "$ROOT/build/libaether_sandbox.so" ]; then
    echo "  [SKIP] sandbox_empty_grants: sandbox runtime not built"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT

if ! ${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror \
        -o "$TMP/child" "$SCRIPT_DIR/child.c" 2>"$TMP/cc.err"; then
    echo "  [FAIL] sandbox_empty_grants: child did not compile"
    sed 's/^/          /' "$TMP/cc.err" | head -8
    exit 1
fi
if ! ${CC:-cc} -std=c11 -Wall -Wextra -Werror \
        -o "$TMP/harness" "$SCRIPT_DIR/harness.c" \
        "$ROOT/build/libaether.a" -lpthread -lm 2>"$TMP/cc.err"; then
    if ! ${CC:-cc} -std=c11 -Wall -Wextra -Werror \
            -o "$TMP/harness" "$SCRIPT_DIR/harness.c" \
            "$ROOT/build/libaether.a" \
            -lpthread -ldl -lm -lrt -lz -lssl -lcrypto 2>"$TMP/cc.err"; then
        echo "  [FAIL] sandbox_empty_grants: harness did not link"
        sed 's/^/          /' "$TMP/cc.err" | head -8
        exit 1
    fi
fi

printf 'secret\n' > "$TMP/data.txt"

# SANDBOX_PRELOAD_OVERRIDE is for proving the test can fail: point it at an
# inert library and the child runs unconfined (see the commit message).
PRELOAD="${SANDBOX_PRELOAD_OVERRIDE:-$ROOT/build/libaether_sandbox.so}"
out="$(AETHER_SANDBOX_PRELOAD="$PRELOAD" AETHER_SANDBOX_LOG=none \
       "$TMP/harness" "$TMP/child" "$TMP/data.txt" 2>&1 </dev/null)"
rc=$?
if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q \
        '^PASS: empty grant list confines the child'; then
    echo "  [PASS] empty grant list refuses read, connect and exec; matching grants allow them"
    echo "PASS: sandbox_empty_grants"
    exit 0
fi

echo "  [FAIL] empty grant list did not confine the child, or the control was refused"
printf '%s\n' "$out" | sed 's/^/          /' | head -30
echo "FAIL: sandbox_empty_grants"
exit 1
