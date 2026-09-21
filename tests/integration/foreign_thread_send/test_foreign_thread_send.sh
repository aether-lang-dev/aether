#!/bin/sh
# A `!` send from a thread that is neither a scheduler core nor the main
# thread reaches the actor (#2083).
#
# Every non-scheduler producer — the main thread, a std.http pool worker,
# a std.worker thread, a C library's callback thread — enqueues into the
# target core's from_queues[MAX_CORES] channel. That channel is SPSC: its
# producer side tolerates one writer. Two foreign threads (or one and the
# main thread) writing it at once raced on `tail`, and messages were
# silently lost: the receive arm never ran, no error anywhere. The channel
# now has a lock that serializes its non-scheduler producers.
#
# A second thing hid behind that: with ONE actor the runtime is in
# main-thread mode — the thread running main() steps the actor inline at
# each send — and a foreign thread's send stepped the same actor at the
# same time (heap corruption). A thread that is not main now leaves the
# mode, as spawning a second actor does, and sends through the scheduler;
# the inline step holds the actor's step_lock so the switch is safe.
#
# Two worker threads and the main thread each send `PER` Bump messages to
# one counter actor at the same time; the counter must end at 3 * PER.
# Run twice: with the counter as the only actor (main-thread mode on:
# crashed before the fix) and with a second actor spawned first (mode
# off: sends into the 1024-slot channel from three threads collided on
# nearly every run, thousands lost).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

if [ ! -x "$AE" ]; then
    echo "  [SKIP] foreign_thread_send: $AE not built"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" || true' EXIT

cat > "$tmp/main.ae" <<'AE'
import std.actors
import std.worker

message Bump {}
message Ask { from: actor_ref }
message Tell { value: int }
message Drain { sink: ptr }

actor Counter {
    state count = 0
    receive {
        Bump() -> { count = count + 1 }
        Ask(from) -> { from ! Tell { value: count } }
    }
}

actor Spy {
    state last = -1
    receive {
        Tell(value) -> { last = value }
        Drain(sink) -> { ref_set(sink, last) }
    }
}

const PER = 20000
const MODE = @MODE@

// Runs on a std.worker thread: a foreign producer.
blast(job: ptr) -> ptr {
    a = actors.whereis("counter")
    for (i = 0; i < PER; i = i + 1) {
        a ! Bump {}
    }
    return null
}

// Every Bump sent before the Ask is counted before the Ask is answered
// (a mailbox is FIFO); what varies is how soon the scheduler gets there.
read_count(target: actor_ref, timeout_ms: int) -> int {
    spy = spawn(Spy())
    target ! Ask { from: spy }
    cell = ref(-1)
    for (waited = 0; waited < timeout_ms; waited = waited + 5) {
        spy ! Drain { sink: cell }
        if ref_get(cell) != -1 { return ref_get(cell) }
        sleep(5)
    }
    return ref_get(cell)
}

main() {
    if MODE == 1 {
        // A second actor first: the runtime is not in main-thread mode.
        _ = spawn(Spy())
    }
    a = spawn(Counter())
    actors.register("counter", a)
    worker.run(|| { return blast(null) }, |r: ptr| { })
    worker.run(|| { return blast(null) }, |r: ptr| { })
    // The main thread is the third producer, concurrently.
    for (i = 0; i < PER; i = i + 1) {
        a ! Bump {}
    }
    waited = 0
    while worker.pending() > 0 && waited < 20000 {
        _ = worker.drain(0)
        sleep(1)
        waited = waited + 1
    }
    _ = worker.drain(0)
    got = read_count(a, 20000)
    println("counted ${got} of ${3 * PER}")
}
AE
fail=0
for mode in 0 1; do
    sed "s/@MODE@/$mode/" "$tmp/main.ae" > "$tmp/run.ae"
    out="$(AETHER_HOME="$ROOT" "$AE" run "$tmp/run.ae" 2>&1)"
    got="$(printf '%s\n' "$out" | tail -1)"
    if [ "$got" != "counted 60000 of 60000" ]; then
        if [ "$mode" = 0 ]; then
            echo "  [FAIL] foreign_thread_send: with one actor (main-thread mode) sends from worker threads were lost or crashed"
        else
            echo "  [FAIL] foreign_thread_send: with two actors (scheduler mode) sends from worker threads were lost or crashed"
        fi
        printf '%s\n' "$out" | tail -4 | sed 's/^/        /'
        fail=1
    fi
done
if [ "$fail" = 0 ]; then
    echo "  [PASS] foreign_thread_send: sends from worker threads and the main thread all arrive, in both runtime modes"
fi
exit $fail
