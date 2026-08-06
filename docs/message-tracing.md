# Message tracing

When an actor program misbehaves, the question is almost always "what actually
happened, in what order, and on which core". Message tracing answers it by
recording every step a message takes through the runtime and writing the result
as JSONL.

```bash
ae build --trace myprog.ae -o myprog
AETHER_TRACE=trace.jsonl ./myprog
```

```json
{"ts_ns":221234006688000,"core":-1,"event":"send_remote","actor":1,"msg":0,"msg_name":"Ping","sender":0}
{"ts_ns":221234006692000,"core":-1,"event":"step_begin","actor":1,"msg":-1}
{"ts_ns":221234006697000,"core":-1,"event":"step_end","actor":1,"msg":-1}
{"summary":true,"events":9,"dropped":0,"capacity_per_core":65536}
```

## It is absent unless you ask for it

Tracing is a **compile-time** feature, not a runtime switch. A normal build
contains no tracing code: the hooks compile to `((void)0)` and the tracing
translation unit compiles to nothing.

This is not a claim about the cost being small. Building
`runtime/scheduler/multicore_scheduler.c` before and after the tracing hooks
were added produces a **byte-identical object file** (same size, same
disassembly), because the macros vanish before the optimiser ever sees them.
Message send is the runtime's core loop, and that is the only way to promise it
is untouched.

`ae build --trace` compiles both your program and the runtime with
`-DAETHER_TRACE`. Because a prebuilt `libaether.a` was compiled without the
flag, a traced build compiles the runtime from source; that is why it takes
longer than a normal build.

The build cache distinguishes traced from untraced builds, so `--trace` after a
normal build of the same source rebuilds rather than handing back the cached
untraced binary.

## Turning it on at run time

A traced binary still writes nothing until `AETHER_TRACE` names an output file.
That keeps a traced build usable for ordinary runs while you are iterating.

| Variable | Meaning |
|---|---|
| `AETHER_TRACE=<path>` | Write the trace to `<path>`. Unset means record nothing. |

## Events

| Event | Meaning |
|---|---|
| `send_local` | `scheduler_send_local` entered, the same-core path |
| `send_remote` | `scheduler_send_remote` entered, the cross-core path |
| `spsc_enqueue` | delivered through an `auto_process` actor's SPSC queue |
| `mailbox_send` | delivered through the actor's mailbox |
| `receive` | dequeued for processing |
| `step_begin` / `step_end` | the actor's `step()` ran |
| `drop_dead` | dropped because the recipient was already dead |

The queue choices are separate events rather than a flag on one "sent" event,
because "which path did this actually take" is usually the question a trace is
opened to answer. A message that went through the SPSC queue and one that went
through the mailbox are exactly the two cases whose difference matters.

Each event carries the timestamp, the core (`-1` is the main thread), the actor
id, and the message id with its name.

## Where names come from

The runtime only ever sees a message as an integer: the id the message registry
assigned. The compiler knows the names, so it emits an id-to-name table into
the generated C and registers it at startup. Without that a trace reads as bare
ordinals, which is not much better than no trace.

The table is emitted inside the `AETHER_TRACE` guard, so an ordinary build
carries neither the strings nor the registration call.

## Cost when it is on

One timestamp and one 32-byte append to a per-core ring buffer. There are no
locks and no atomics on the hot path: a core only ever writes its own buffer,
and the buffers are read after `scheduler_shutdown()` has joined every thread,
so the join supplies the ordering the merge needs.

The trace is written from `scheduler_shutdown()` rather than `atexit`, which
keeps the exit path free of the hang that teardown-at-exit caused for the
worker pool.

## Completeness

Each core's buffer holds 65536 events (2 MiB) and wraps, keeping the most
recent events. The trailing `summary` line reports how many events were written
and how many were dropped:

```json
{"summary":true,"events":9,"dropped":0,"capacity_per_core":65536}
```

A non-zero `dropped` means the ring wrapped and the beginning of the run is
gone. It is reported rather than inferred, because a truncated trace that looks
complete is worse than no trace. Raise the capacity at build time with
`-DAETHER_TRACE_CAPACITY=<power of two>` if you need a longer window.

## Reading a trace

JSONL, one event per line, ordered by timestamp across all cores, because a
trace is read with `grep` and `jq` far more often than with a viewer.

```bash
# every message a given actor received
jq -c 'select(.actor == 3)' trace.jsonl

# just the sends, by name
jq -r 'select(.msg_name) | .msg_name' trace.jsonl | sort | uniq -c

# was anything dropped?
jq -c 'select(.summary)' trace.jsonl
```
