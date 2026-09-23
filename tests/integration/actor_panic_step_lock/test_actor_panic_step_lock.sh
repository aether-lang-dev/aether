#!/bin/sh
# #2163: nothing releases an actor's step_lock except whoever took it.
#
# `aether_step_safe` released `step_lock` on its panic path without ever
# having acquired it. All seven call sites take it before calling and clear
# it after, so the panic path handed the lock away while the caller still
# believed it held it -- another thread could acquire and step the same
# actor, the caller then cleared a lock it no longer owned, and a third
# could enter. Two threads in one actor's step is heap corruption, seen as
# `free(): invalid pointer` from the #2083 regression test on CI.
#
# This has two halves, and they do different jobs.
#
# The INVARIANT below is deterministic: it reads the runtime source and
# fails if that function releases the lock again. A race cannot be pinned
# by running a program -- an absence of crashes is not proof -- but a rule
# about who owns a lock can be pinned by reading, and that is the rule that
# was broken.
#
# The STRESS half exercises the path: an actor that panics while two worker
# threads and the main thread send to it, with an allocation inside the
# step so concurrent unwinding corrupts the allocator instead of racing an
# integer quietly. It earns its keep on the ASan and Valgrind lanes.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

[ -x "$AE" ] || { echo "  [SKIP] actor_panic_step_lock: ae not built"; exit 0; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT
fail=0

# --- the invariant, read from the source -----------------------------------
src="$ROOT/runtime/scheduler/multicore_scheduler.c"
body="$(awk '/^static inline void aether_step_safe/,/^}/' "$src")"
# An invariant read out of a source file has to prove it read the right
# thing first: if the function is renamed and this extracts nothing, the
# check below would pass forever while guarding nothing at all.
if ! printf '%s\n' "$body" | grep -q 'aether_fire_death_hook'; then
    echo "  [FAIL] actor_panic_step_lock: could not find aether_step_safe's panic path in $src"
    echo "         (the function was renamed or moved -- this check is guarding nothing until it is repointed)"
    fail=1
fi
if printf '%s\n' "$body" | grep -q 'atomic_flag_clear_explicit(&actor->step_lock'; then
    echo "  [FAIL] actor_panic_step_lock: aether_step_safe releases a step_lock it never acquired"
    echo "         every call site takes the lock before calling and clears it after;"
    echo "         releasing it here hands it away mid-step (#2163)"
    fail=1
fi
# The callers must still each release exactly once, or the lock leaks and
# the actor is never stepped again.
releases=$(grep -c 'atomic_flag_clear_explicit(&actor->step_lock' "$src")
if [ "$releases" -lt 4 ]; then
    echo "  [FAIL] actor_panic_step_lock: only $releases step_lock release(s) in the scheduler; the callers should each have one"
    fail=1
fi

# --- the stress half -------------------------------------------------------
cd "$SCRIPT_DIR" || exit 1
if ! "$AE" build probe.ae -o "$tmp/probe" > "$tmp/build.log" 2>&1; then
    echo "  [FAIL] actor_panic_step_lock: build failed"
    sed 's/^/        /' "$tmp/build.log" | head -10
    exit 1
fi

runs=3
i=0
while [ "$i" -lt "$runs" ]; do
    i=$((i + 1))
    out="$("$tmp/probe" 2>&1)"
    rc=$?
    # The actor's panic is expected and is not a failure; the process
    # surviving it, with its heap intact, is the point.
    if [ "$rc" -ne 0 ] || ! printf '%s\n' "$out" | grep -q '^survived$'; then
        echo "  [FAIL] actor_panic_step_lock: run $i of $runs did not survive the panic (exit $rc)"
        printf '%s\n' "$out" | tail -4 | sed 's/^/        /'
        fail=1
        break
    fi
done

if [ "$fail" = 0 ]; then
    echo "  [PASS] actor_panic_step_lock: the panic path releases no lock it did not take; $runs concurrent-panic runs survived"
fi
exit $fail
