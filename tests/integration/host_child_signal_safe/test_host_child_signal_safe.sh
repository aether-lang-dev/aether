#!/bin/sh
# Regression (#3200): contrib's process host (contrib/host/aether) prepares
# its child in the parent, and the child calls no setenv between fork and
# exec. See harness.c. (The Go host moved to aether-guest-languages.)
#
# Linux only: the host's sandboxed runs need libaether_sandbox.so in
# LD_PRELOAD, which macOS's System Integrity Protection strips, and
# elsewhere the host is a stub.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s 2>/dev/null)" in
    Linux) ;;
    *)
        echo "  [SKIP] host_child_signal_safe: the host's sandboxed runs are Linux only"
        exit 0 ;;
esac
if [ ! -f "$ROOT/build/libaether.a" ]; then
    echo "  [SKIP] host_child_signal_safe: build/libaether.a not built"
    exit 0
fi
if [ ! -f "$ROOT/build/libaether_sandbox.so" ]; then
    echo "  [SKIP] host_child_signal_safe: build/libaether_sandbox.so not built"
    exit 0
fi
TRUE_PATH="$(command -v true)"
case "$TRUE_PATH" in
    /*) ;;
    *) TRUE_PATH=/bin/true ;;
esac

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

INC="-I$ROOT/runtime -I$ROOT/runtime/utils -I$ROOT/runtime/config -I$ROOT/std"
SRC="$SCRIPT_DIR/harness.c $ROOT/contrib/host/aether/aether_host_aether.c"
# shellcheck disable=SC2086
if ! ${CC:-cc} $INC -I"$ROOT/contrib/host/aether" \
        -o "$TMP/harness" $SRC "$ROOT/build/libaether.a" -lpthread -lm 2>"$TMP/cc.err"; then
    # The runtime links more libraries on some platforms; try the full set.
    # shellcheck disable=SC2086
    if ! ${CC:-cc} $INC -I"$ROOT/contrib/host/aether" \
            -o "$TMP/harness" $SRC "$ROOT/build/libaether.a" \
            -lpthread -ldl -lm -lrt -lz -lssl -lcrypto 2>"$TMP/cc.err"; then
        echo "  [FAIL] could not link the harness against the host and libaether.a"
        sed 's/^/          /' "$TMP/cc.err" | head -8
        exit 1
    fi
fi

mkdir -p "$TMP/work"
out="$(AETHER_SANDBOX_PRELOAD="$ROOT/build/libaether_sandbox.so" \
       "$TMP/harness" "$TMP/work" "$TRUE_PATH" 2>&1 </dev/null)"
rc=$?
if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q "PASS: the process host's children call only async-signal-safe functions"; then
    echo "  [PASS] the process host's children call only async-signal-safe functions"
    echo "PASS: host_child_signal_safe"
    exit 0
fi
echo "  [FAIL] the process host's child called setenv, or ran with the wrong env or program"
printf '%s\n' "$out" | sed 's/^/          /' | head -20
echo "FAIL: host_child_signal_safe"
exit 1
