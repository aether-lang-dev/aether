#!/bin/sh
# Two concurrent spawn_sandboxed calls must not publish the same grant
# transport. The harness holds both callers at aether_fork until both shared
# memory objects have been created, then compares the names seen by the children.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    Linux|FreeBSD) ;;
    *)
        echo "  [SKIP] sandbox_unique_shm: spawn_sandboxed is Linux and FreeBSD only"
        exit 0 ;;
esac
if [ ! -f "$ROOT/build/libaether.a" ]; then
    echo "  [SKIP] sandbox_unique_shm: build/libaether.a not built"
    exit 0
fi
if [ ! -f "$ROOT/build/libaether_sandbox.so" ]; then
    echo "  [SKIP] sandbox_unique_shm: build/libaether_sandbox.so not built"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT

if ! ${CC:-cc} -o "$TMP/harness" "$SCRIPT_DIR/harness.c" \
        "$ROOT/build/libaether.a" -Wl,--wrap=aether_fork \
        -lpthread -lm 2>"$TMP/cc.err"; then
    if ! ${CC:-cc} -o "$TMP/harness" "$SCRIPT_DIR/harness.c" \
            "$ROOT/build/libaether.a" -Wl,--wrap=aether_fork \
            -lpthread -ldl -lm -lrt -lz -lssl -lcrypto 2>"$TMP/cc.err"; then
        echo "  [FAIL] could not link the concurrent spawn harness"
        sed 's/^/          /' "$TMP/cc.err" | head -8
        exit 1
    fi
fi

mkdir -p "$TMP/work"
out="$(AETHER_SANDBOX_PRELOAD="$ROOT/build/libaether_sandbox.so" \
       env -u AETHER_SANDBOX_VERBOSE "$TMP/harness" "$TMP/work" 2>&1 </dev/null)"
rc=$?
if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q \
        "PASS: concurrent spawn_sandboxed calls use separate shared memory"; then
    echo "  [PASS] concurrent spawn_sandboxed calls use separate shared memory"
    echo "PASS: sandbox_unique_shm"
    exit 0
fi

echo "  [FAIL] concurrent spawn_sandboxed calls shared or leaked grant transport"
printf '%s\n' "$out" | sed 's/^/          /' | head -20
echo "FAIL: sandbox_unique_shm"
exit 1
