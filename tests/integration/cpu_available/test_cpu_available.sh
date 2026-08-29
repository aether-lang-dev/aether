#!/bin/sh
# Thread pools are sized from the CPUs the process may actually use. Sizing
# them from the machine's CPU count oversubscribes every CPU-limited
# deployment, and nothing about the machine's count changes when a limit is
# applied, so only running under a limit can catch a regression.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
TMPDIR="$(mktemp -d)"
cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT

CC="${CC:-cc}"
command -v "$CC" >/dev/null 2>&1 || { echo "  [SKIP] no C compiler"; exit 0; }

cat > "$TMPDIR/probe.c" <<'EOF'
#include "aether_cpu_available.h"
#include <stdio.h>
int main(void) { printf("%d\n", aether_cpu_available()); return 0; }
EOF

if ! "$CC" -O2 -I"$ROOT/runtime/utils" -D_GNU_SOURCE \
        -o "$TMPDIR/probe" "$TMPDIR/probe.c" >"$TMPDIR/cc.log" 2>&1; then
    echo "  [FAIL] probe did not build:"; cat "$TMPDIR/cc.log"; exit 1
fi

fail=0
N=$("$TMPDIR/probe")
case "$N" in
    ''|*[!0-9]*) echo "  [FAIL] unconstrained: not a number: '$N'"; exit 1 ;;
esac
[ "$N" -ge 1 ] || { echo "  [FAIL] unconstrained: expected >= 1, got $N"; fail=1; }

# An affinity mask is the one limit that can be applied from a shell without
# root or a container runtime.
if command -v taskset >/dev/null 2>&1 && [ "$N" -ge 2 ]; then
    ONE=$(taskset -c 0 "$TMPDIR/probe")
    [ "$ONE" = "1" ] || { echo "  [FAIL] taskset -c 0: expected 1, got $ONE"; fail=1; }
    TWO=$(taskset -c 0,1 "$TMPDIR/probe")
    [ "$TWO" = "2" ] || { echo "  [FAIL] taskset -c 0,1: expected 2, got $TWO"; fail=1; }
    [ "$fail" = "0" ] && echo "  [PASS] cpu_available: 3/3 - unconstrained $N, cpuset 1 and 2 honoured"
else
    [ "$fail" = "0" ] && echo "  [PASS] cpu_available: 1/1 - unconstrained $N (no taskset; cpuset cases skipped)"
fi

exit $fail
