#!/bin/sh
# The runtime's public headers are valid C11 (#3548): each one compiles on
# its own under -std=c11 -pedantic -Werror, as a host that embeds the runtime
# may build it. The headers are the embedder's (include/libaether.h) and every
# runtime header the generated C names. AetherOptFlags ended in
# AETHER_OPT_ALL = 0xFFFFFFFF, which does not fit in an int, the type of an
# enumerator before C23, so every header that reached
# aether_optimization_config.h (the actor runtime's, multicore_scheduler.h,
# aether_actor_thread.h) was refused.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$ROOT" || exit 1

CC_BIN="${CC:-cc}"
if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    CC_BIN=gcc
fi
if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    echo "  [SKIP] runtime_headers_strict_c11: no C compiler"
    exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

INC="-I. -Iinclude -Iruntime -Iruntime/utils -Iruntime/actors -Iruntime/config -Iruntime/scheduler -Iruntime/memory -Istd -Istd/collections -Istd/string"
# -std=c11 hides POSIX on glibc and musl; strict C11 code that uses POSIX
# asks for it, as tests/compiler/test_msvc_compat.sh does.
STRICT_POSIX=""
case "$(uname -s 2>/dev/null)" in
    Linux*) STRICT_POSIX="-D_POSIX_C_SOURCE=200809L" ;;
esac

HEADERS="libaether.h aether_abi.h aether_panic.h aether_slice.h aether_stringseq.h
    aether_schema.h actor_state_machine.h aether_send_message.h aether_actor_thread.h
    multicore_scheduler.h aether_cpu_detect.h aether_optimization_config.h
    aether_bounds_check.h aether_runtime_types.h aether_compiler.h"

fail=0
for h in $HEADERS; do
    name="$(basename "$h" .h)"
    printf '#include "%s"\n' "$h" > "$TMP/$name.c"
    # shellcheck disable=SC2086
    if "$CC_BIN" -fsyntax-only -std=c11 -pedantic -Werror $STRICT_POSIX $INC \
            "$TMP/$name.c" > "$TMP/$name.log" 2>&1; then
        echo "  [PASS] $h compiles under -std=c11 -pedantic -Werror"
    else
        grep -v "In file included\|^ *from " "$TMP/$name.log" | sed 's/^/          /' | head -10
        echo "  [FAIL] $h does not compile under -std=c11 -pedantic -Werror"
        fail=1
    fi
done

# The value itself: every flag bit set, and an int.
cat > "$TMP/all.c" <<'EOF'
#include <limits.h>
#include <stdio.h>
#include "aether_optimization_config.h"
AetherRuntimeConfig g_aether_config;
int main(void) {
    int all = AETHER_OPT_ALL;
    int flags[3] = { AETHER_OPT_LOCKFREE_MAILBOX, AETHER_OPT_MESSAGE_DEDUP, AETHER_OPT_VERBOSE };
    for (int i = 0; i < 3; i++) {
        if ((all & flags[i]) != flags[i]) { printf("flag %d not in AETHER_OPT_ALL\n", flags[i]); return 1; }
    }
    if (all != INT_MAX) { printf("AETHER_OPT_ALL is %d\n", all); return 1; }
    aether_enable_opt(AETHER_OPT_ALL);
    if (!aether_has_opt(AETHER_OPT_ALL)) { printf("aether_has_opt(AETHER_OPT_ALL) after enabling it\n"); return 1; }
    printf("ok\n");
    return 0;
}
EOF
# shellcheck disable=SC2086
if ! "$CC_BIN" -std=c11 -pedantic -Werror $STRICT_POSIX $INC -o "$TMP/all" "$TMP/all.c" \
        > "$TMP/all.log" 2>&1; then
    sed 's/^/          /' "$TMP/all.log" | head -10
    echo "  [FAIL] a program using AETHER_OPT_ALL does not build"
    fail=1
elif [ "$("$TMP/all" | tr -d '\r')" != "ok" ]; then
    "$TMP/all" | sed 's/^/          /'
    echo "  [FAIL] AETHER_OPT_ALL does not hold every flag"
    fail=1
else
    echo "  [PASS] AETHER_OPT_ALL is an int with every flag bit set"
fi

if [ "$fail" -ne 0 ]; then
    echo "FAIL: runtime_headers_strict_c11"
    exit 1
fi
echo "PASS: runtime_headers_strict_c11"
exit 0
