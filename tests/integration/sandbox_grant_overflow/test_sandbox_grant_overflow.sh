#!/bin/sh
# An oversized grant list must deny the spawn instead of overrunning the
# serialisation buffer. Built with ASan when the compiler supports it.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
. "$ROOT/tests/lib/syslibs.sh"

case "$(uname -s 2>/dev/null)" in
    Linux|FreeBSD) ;;
    *)
        echo "  [SKIP] sandbox_grant_overflow: spawn_sandboxed is Linux and FreeBSD only"
        exit 0 ;;
esac
if [ ! -f "$ROOT/build/libaether.a" ] || [ ! -f "$ROOT/build/libaether_sandbox.so" ]; then
    echo "  [SKIP] sandbox_grant_overflow: runtime or preload library not built"
    exit 0
fi

TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT

built=0
for san in "-fsanitize=address" ""; do
    for libs in "-lpthread -lm" "-lpthread $AE_DL_LIBS -lm $AE_RT_LIBS -lz -lssl -lcrypto"; do
        # shellcheck disable=SC2086
        if ${CC:-cc} $san -o "$TMP/harness" "$SCRIPT_DIR/harness.c" \
                "$ROOT/build/libaether.a" $libs 2>"$TMP/cc.err"; then
            built=1; break 2
        fi
    done
done
if [ "$built" -ne 1 ]; then
    echo "  [FAIL] sandbox_grant_overflow: could not link harness"
    sed 's/^/          /' "$TMP/cc.err" | head -8
    exit 1
fi

# The harness deliberately leaks its grant list; only the overrun matters.
out="$(ASAN_OPTIONS=detect_leaks=0 AETHER_SANDBOX_PRELOAD="$ROOT/build/libaether_sandbox.so" \
       "$TMP/harness" 2>&1 </dev/null)"
rc=$?
if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q "PASS: oversized grant list denied"; then
    echo "  [PASS] oversized grant list denied without overrun"
    echo "PASS: sandbox_grant_overflow"
    exit 0
fi
echo "  [FAIL] oversized grant list not denied cleanly (rc=$rc)"
printf '%s\n' "$out" | head -12 | sed 's/^/          /'
exit 1
