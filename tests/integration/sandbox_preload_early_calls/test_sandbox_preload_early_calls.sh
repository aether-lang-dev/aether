#!/bin/sh
# The sandbox preload does not call into the program before its own
# constructor has finished.
#
# A sanitizer's runtime starts before every constructor and calls libc as it
# does: TSan makes the directory of its log_path with mkdir from
# .preinit_array, and that mkdir is the preload's. The preload asked the
# program's in-process policy hook (aether_sandbox_levels_here) from there,
# and in a TSan build the hook is instrumented code, which faulted inside
# TSan's own start (TraceSwitchPart): http_ipv6 died with SIGSEGV on #3768's
# TSan leg, under the sweep's log_path. probe.c makes the same early call,
# with a hook that stops the program (exit 3) if it is asked before main,
# so the order is checked on every build, sanitizer or not: with no grants,
# and with a grant file in force.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
if [ "$(uname -s 2>/dev/null)" != Linux ]; then
    echo "  [SKIP] sandbox_preload_early_calls: .preinit_array and the preload are Linux here"
    exit 0
fi
CC_PROBE="${CC:-cc}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" || true' EXIT

# The preload from source, as sandbox_spawn_family builds it, so the test
# holds the source it runs against.
if ! $CC_PROBE -shared -fPIC -o "$TMP/libaether_sandbox.so" \
        "$ROOT/runtime/libaether_sandbox_preload.c" -ldl -lrt > "$TMP/cc.log" 2>&1; then
    echo "  [FAIL] sandbox_preload_early_calls: the preload does not build"
    sed 's/^/    /' "$TMP/cc.log"
    exit 1
fi
if ! $CC_PROBE -rdynamic -o "$TMP/probe" "$SCRIPT_DIR/probe.c" > "$TMP/cc.log" 2>&1; then
    echo "  [FAIL] sandbox_preload_early_calls: probe.c does not build"
    sed 's/^/    /' "$TMP/cc.log"
    exit 1
fi

fail=0
run() {   # $1 label, $2 grant file or ""
    rm -rf "$TMP/made"
    rc=0
    out=$(LD_PRELOAD="$TMP/libaether_sandbox.so" AETHER_SANDBOX_LOG=none \
          AETHER_SANDBOX_GRANTS="$2" "$TMP/probe" "$TMP/made" 2>&1) || rc=$?
    if [ "$rc" = 0 ] && [ "$out" = ok ]; then
        echo "  [PASS] sandbox_preload_early_calls: $1"
    else
        echo "  [FAIL] sandbox_preload_early_calls: $1: exit $rc"
        printf '%s\n' "$out" | sed 's/^/    /'
        fail=1
    fi
}
run "no grants" ""
# A grant file: the policy is in force once the constructor has read it,
# and not before.
printf 'fs_read:/*\nfs_write:%s/*\n' "$TMP" > "$TMP/grants"
run "a grant file" "$TMP/grants"
exit $fail
