#!/bin/sh
# Regression (#3200): a child spawn_sandboxed starts calls nothing between
# fork and exec that is not async-signal-safe: no setenv, no execlp, no
# stdio. The parent builds the env and resolves the program. See harness.c.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/syslibs.sh"

case "$(uname -s 2>/dev/null)" in
    Linux|FreeBSD) ;;
    *)
        echo "  [SKIP] sandbox_child_signal_safe: spawn_sandboxed is Linux and FreeBSD only"
        exit 0 ;;
esac
if [ ! -f "$ROOT/build/libaether.a" ]; then
    echo "  [SKIP] sandbox_child_signal_safe: build/libaether.a not built"
    exit 0
fi
if [ ! -f "$ROOT/build/libaether_sandbox.so" ]; then
    echo "  [SKIP] sandbox_child_signal_safe: build/libaether_sandbox.so not built"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT

if ! ${CC:-cc} -o "$TMP/harness" "$SCRIPT_DIR/harness.c" \
        "$ROOT/build/libaether.a" -lpthread -lm 2>"$TMP/cc.err"; then
    # The runtime links more libraries on some platforms; try the full set.
    if ! ${CC:-cc} -o "$TMP/harness" "$SCRIPT_DIR/harness.c" \
            "$ROOT/build/libaether.a" -lpthread $AE_DL_LIBS -lm $AE_RT_LIBS -lz -lssl -lcrypto 2>"$TMP/cc.err"; then
        echo "  [FAIL] could not link the harness against libaether.a"
        sed 's/^/          /' "$TMP/cc.err" | head -8
        exit 1
    fi
fi

mkdir -p "$TMP/work"
out="$(AETHER_SANDBOX_PRELOAD="$ROOT/build/libaether_sandbox.so" \
       env -u AETHER_SANDBOX_VERBOSE "$TMP/harness" "$TMP/work" 2>&1 </dev/null)"
rc=$?
if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q "PASS: spawn_sandboxed's children call only async-signal-safe functions"; then
    echo "  [PASS] spawn_sandboxed's children call only async-signal-safe functions"
    echo "PASS: sandbox_child_signal_safe"
    exit 0
fi
echo "  [FAIL] a spawn_sandboxed child called setenv, or ran with the wrong env or program"
printf '%s\n' "$out" | sed 's/^/          /' | head -20
echo "FAIL: sandbox_child_signal_safe"
exit 1
