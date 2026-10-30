#!/bin/sh
# Issue #343 part 3/3 — codegen tripwire end-to-end.
#
# Verifies:
#   1. `aetherc --emit=lib` generates C with a deadline check at
#      every loop head.
#   2. Linking the lib together with the runtime caps surface and
#      arming a deadline causes spin(huge_iters) to return inside
#      the deadline window — even though the loop would otherwise
#      run for many seconds.
#   3. `aetherc --emit=exe` on the same source emits ZERO
#      references to aether_caps_deadline_tripped or aether_exec_poll
#      (the loop-head safe point since #2956), proving the gate
#      elides the tripwire on exe builds (`--emit=exe` pays nothing).
#   4. A function that returns a value and ends in a loop nothing leaves
#      (forever.ae) compiles under -Werror=return-type and, ended at its
#      deadline, returns its zero value: the C does not fall off its end.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AETHERC="$ROOT/build/aetherc"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR" || true' EXIT

# --- Step 1: emit the loop fixture as C in --emit=lib mode. ---
"$AETHERC" --emit=lib "$SCRIPT_DIR/loop.ae" "$TMPDIR/loop_lib.c" \
    >"$TMPDIR/build_lib.log" 2>&1 || {
    echo "  [FAIL] aetherc --emit=lib build:"
    head -30 "$TMPDIR/build_lib.log"
    exit 1
}

# The loop-head safe point must appear >=2 times: the extern decl in the
# program prelude + at least one call at a loop head (#2956: the
# safe point asks aether_exec_poll(), which also sees a cancel).
TRIPWIRE_COUNT=$(grep -c "aether_exec_poll" "$TMPDIR/loop_lib.c" || true)
[ "$TRIPWIRE_COUNT" -ge 2 ] || {
    echo "  [FAIL] tripwire emission: expected ≥2 (extern decl + ≥1 loop site), got $TRIPWIRE_COUNT"
    exit 1
}

# --- Step 2: compile the emitted C with the cap runtime + a
# minimal C harness that arms the deadline and calls spin(). ---
cat >"$TMPDIR/harness.c" <<'EOF'
#include <stdio.h>
#include <time.h>
#include "../include/libaether.h"

extern int spin(int iters);

int main(void) {
    /* 1 ms deadline. The spin loop runs 1 billion iterations
     * uncapped. With the tripwire, __aether_abort_call sets the
     * sticky flag at the first loop head past the deadline; the
     * subsequent loop head returns from spin via the codegen's
     * abort path (which exits the loop body — the function
     * returns its current accumulator). */
    aether_set_call_deadline(1);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int r = spin(1000000000);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ns = (t1.tv_sec - t0.tv_sec) * 1000000000L
            + (t1.tv_nsec - t0.tv_nsec);
    long ms = ns / 1000000L;
    /* Print result + elapsed for the shell to verify. The deadline
     * was 1 ms; allow up to 200 ms of harness startup + the loops
     * between tripwire checks. Anything above 1 second means the
     * tripwire didn't fire. */
    printf("ELAPSED_MS=%ld RESULT=%d\n", ms, r);
    return ms < 1000 ? 0 : 1;
}
EOF

# Compile the emitted .c, the caps runtime, and the harness together.
# The harness includes libaether.h via -I; the runtime files come
# straight from runtime/.
# Linker order matters on Linux: archive members are pulled in
# only when an already-seen object file references them. -laether
# placed before harness.c (which calls aether_set_call_deadline)
# would discard the cap symbols. Two ways to fix:
#   (a) Move -laether AFTER harness.c.
#   (b) Compile the cap modules directly — sidesteps archive rules
#       entirely.
# Going with (a) here so loop_lib.c (which may reference std.string
# helpers via the emitted prelude) still resolves cleanly against
# libaether.a — the cap symbols within the archive are pulled in
# once harness.c's references force the linker to load them.
"${CC:-cc}" -O2 -I"$ROOT" -I"$ROOT/runtime" -I"$ROOT/runtime/actors" \
   -I"$ROOT/runtime/scheduler" -I"$ROOT/runtime/utils" \
   -I"$ROOT/runtime/memory" -I"$ROOT/runtime/config" \
   -I"$ROOT/std" -I"$ROOT/std/string" -I"$ROOT/std/collections" \
   -I"$ROOT/include" \
   "$TMPDIR/loop_lib.c" \
   "$TMPDIR/harness.c" \
   -L"$ROOT/build" -laether \
   -o "$TMPDIR/harness" \
   -lm -pthread \
   2>"$TMPDIR/cc.log" || {
    echo "  [FAIL] cc harness:"
    head -30 "$TMPDIR/cc.log"
    exit 1
}

# Run. The tripwire must abort the spin within 1 second wall-clock.
if ! "$TMPDIR/harness" >"$TMPDIR/run.log" 2>&1; then
    echo "  [FAIL] harness exited non-zero — deadline did not trip in time:"
    cat "$TMPDIR/run.log"
    exit 1
fi
ELAPSED=$(grep -o 'ELAPSED_MS=[0-9]*' "$TMPDIR/run.log" | cut -d= -f2)
[ -n "$ELAPSED" ] || { echo "  [FAIL] no ELAPSED_MS in output"; cat "$TMPDIR/run.log"; exit 1; }
[ "$ELAPSED" -lt 1000 ] || {
    echo "  [FAIL] spin ran for ${ELAPSED}ms (deadline was 1ms)"
    cat "$TMPDIR/run.log"
    exit 1
}

# --- Step 3: same source, --emit=exe — must contain zero
# references to the tripwire (zero-overhead claim). ---
"$AETHERC" --emit=exe "$SCRIPT_DIR/loop.ae" "$TMPDIR/loop_exe.c" \
    >"$TMPDIR/build_exe.log" 2>&1 || {
    echo "  [FAIL] aetherc --emit=exe build:"
    head -30 "$TMPDIR/build_exe.log"
    exit 1
}
EXE_COUNT=$(grep -c -e "aether_caps_deadline_tripped" -e "aether_exec_poll" "$TMPDIR/loop_exe.c" || true)
[ "$EXE_COUNT" = "0" ] || {
    echo "  [FAIL] --emit=exe contains $EXE_COUNT tripwire references (expected 0)"
    exit 1
}


# --- Step 4: a function that returns a value and ends in a loop nothing
# leaves. Ended at a loop head, the call reaches the end of its body, so
# the C returns the result's zero value there instead of falling off the
# end (-Wreturn-type, and a value the caller read undefined). ---
"$AETHERC" --emit=lib "$SCRIPT_DIR/forever.ae" "$TMPDIR/forever_lib.c" \
    >"$TMPDIR/build_forever.log" 2>&1 || {
    echo "  [FAIL] aetherc --emit=lib forever.ae:"
    head -30 "$TMPDIR/build_forever.log"
    exit 1
}
cat >"$TMPDIR/harness_forever.c" <<'EOF2'
#include <stdio.h>
#include "../include/libaether.h"

extern int forever(void);

int main(void) {
    aether_set_call_deadline(1);
    printf("RESULT=%d\n", forever());
    return 0;
}
EOF2
"${CC:-cc}" -O2 -Werror=return-type -I"$ROOT" -I"$ROOT/runtime" -I"$ROOT/runtime/actors" \
   -I"$ROOT/runtime/scheduler" -I"$ROOT/runtime/utils" \
   -I"$ROOT/runtime/memory" -I"$ROOT/runtime/config" \
   -I"$ROOT/std" -I"$ROOT/std/string" -I"$ROOT/std/collections" \
   -I"$ROOT/include" \
   "$TMPDIR/forever_lib.c" \
   "$TMPDIR/harness_forever.c" \
   -L"$ROOT/build" -laether \
   -o "$TMPDIR/harness_forever" \
   -lm -pthread \
   2>"$TMPDIR/cc_forever.log" || {
    echo "  [FAIL] cc -Werror=return-type forever_lib.c:"
    head -30 "$TMPDIR/cc_forever.log"
    exit 1
}
"$TMPDIR/harness_forever" >"$TMPDIR/run_forever.log" 2>&1 || {
    echo "  [FAIL] forever() under a deadline:"
    cat "$TMPDIR/run_forever.log"
    exit 1
}
grep -q '^RESULT=0$' "$TMPDIR/run_forever.log" || {
    echo "  [FAIL] forever() ended at its deadline did not return 0:"
    cat "$TMPDIR/run_forever.log"
    exit 1
}

echo "  [PASS] emit_lib_deadline: tripwire fires within ${ELAPSED}ms; a loop nothing leaves returns its zero value; --emit=exe is zero-overhead"
