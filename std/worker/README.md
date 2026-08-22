# std.worker

An off-scheduler thread pool, for blocking work.

Aether's model is actors, not locks — but a **blocking** call (a synchronous
read, a `sleep`, a long CPU burn) run on a scheduler thread stalls every actor
sharing it. `std.worker` is the escape hatch: hand the blocking work to a
pthread outside the scheduler and get a callback when it finishes.

```aether
import std.worker

main() {
    // pool_size configures the pool; call it before the first run.
    worker.pool_size(4)

    ok = worker.run(| _ctx: ptr | {
        // Runs on a pool thread. Block freely here.
    }, | _ctx: ptr | {
        // Runs on the main thread once the work is done.
        println("finished")
    })

    if !ok {
        println("could not queue the work")
        return
    }

    // wait blocks until the pool is idle; drain runs up to `max`
    // pending completions on this thread and returns how many it ran.
    worker.wait()
    worker.drain(16)

    worker.pool_shutdown()
}
```

The example **compiles but is not run** in CI: it depends on thread scheduling
and would need a deterministic completion order to assert output.

The `done` callback runs on the **main** thread, not the pool thread, which is
what makes it safe to touch actor state or anything else with thread affinity.
The work callback has no such guarantee — treat everything it touches as
thread-local or immutable.

`pending()` reports queued-and-unfinished work, which is how a shutdown path
knows whether to keep draining.

Under `AETHER_NO_THREADING` the pool degrades to running work inline, so a
build without threads still executes rather than deadlocking.

## Exports

`run`, `run_detached`, `map`, `wait`, `drain`, `pending`, `set_main_poster`,
`deliver`, `pool_size`, `pool_shutdown`.
