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
# Two worker threads and the main thread each send `PER` Bump messages to
# one counter actor at the same time; the counter must end at 3 * PER.
# Before the fix, sends into the 1024-slot channel from three threads
# collided on nearly every run and thousands were lost. (The one-actor
# main-thread mode this test also ran, and its races #2083 and #2266, went
# with the mode, #3112.)

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
import std.sync

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
        Drain(sink) -> { sync.atomic_store(sink, last) }
    }
}

const PER = 20000

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
// The spy writes into a std.sync cell this thread polls (a ref is plain
// memory, which the spy's thread and this one raced, #3542), one Drain at
// a time: -2 before each, so none is on its way when the cell is freed.
read_count(target: actor_ref, timeout_ms: int) -> int {
    spy = spawn(Spy())
    target ! Ask { from: spy }
    cell = sync.atomic_new(-1)
    long got = -1
    waited = 0
    while got == -1 && waited < timeout_ms {
        sync.atomic_store(cell, -2)
        spy ! Drain { sink: cell }
        while sync.atomic_load(cell) == -2 && waited < timeout_ms {
            sleep(5)
            waited = waited + 5
        }
        got = sync.atomic_load(cell)
    }
    if got == -2 { return -1 }   // a Drain is on its way: the cell outlives it
    sync.atomic_free(cell)
    int answer = got   // a count, well inside an int
    return answer
}

main() {
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
if ! out="$(AETHER_HOME="$ROOT" "$AE" build "$tmp/main.ae" -o "$tmp/run" 2>&1)"; then
    echo "  [FAIL] foreign_thread_send: the program did not build"
    printf '%s\n' "$out" | tail -4 | sed 's/^/        /'
    exit 1
fi
runs=10
i=0
while [ "$i" -lt "$runs" ]; do
    i=$((i + 1))
    out="$("$tmp/run" 2>&1)"
    got="$(printf '%s\n' "$out" | tail -1)"
    if [ "$got" != "counted 60000 of 60000" ]; then
        echo "  [FAIL] foreign_thread_send: sends from worker threads were lost or crashed (run $i of $runs)"
        printf '%s\n' "$out" | tail -4 | sed 's/^/        /'
        fail=1
        break
    fi
done
if [ "$fail" = 0 ]; then
    echo "  [PASS] foreign_thread_send: sends from worker threads and the main thread all arrive"
fi
exit $fail
