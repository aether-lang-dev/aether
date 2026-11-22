# Task execution contexts: more than one task in a cooperative runtime

Status: implemented (runtime ABI revision 6). Asked for by the ESP32
smart-plug work in `asks/esp-idf-cooperative-task-local-execution-state.md`;
the reply is `asks/REPLY-esp-idf-cooperative-task-local-execution-state.md`.

## The problem

A threaded runtime keeps each thread's *execution state* in one
thread-local block (`AetherFrames`, `runtime/actors/aether_frames.h`):
everything a call into Aether reads or changes about how it runs rather
than what it computes. A cooperative runtime (`AETHER_NO_THREADING`, the
build an ESP32 links) has no thread-locals, on purpose: ESP-IDF allocates a
program's thread-local segment on every FreeRTOS task's stack, Wi-Fi, IPC
and timer tasks included, and Aether's frames overflowed those small stacks
before `app_main` ran. So the block was one plain global, and every
FreeRTOS task that entered Aether shared it.

That is correct for one Aether task. With two (a pure-Aether HTTP/TLS
service beside a RexxAE worker, each under its own execution account), the
tasks crossed each other's state. Reproduced on 0.935.0 with
`tests/integration/coop_task_contexts` built without a port (POSIX threads
standing in for tasks, the cooperative runtime as the ESP32 builds it): 29
of 30 runs failed, pinned to one core or not. A deadline or panic on one
task longjmp'd into the other task's stack (SIGSEGV, SIGBUS), a cancel
reached the wrong call, and caller identities crossed in 60 of 60 rounds.

## The design

**One block per task, reached through one accessor.** `AetherFrames` holds
all of a thread's execution state (the first commit of this work gathered
the pieces that were thread-locals of their own files into it). On a
threaded build it stays the thread-local it was. On a cooperative build it
is the *built-in context*, one per program, until a host installs a
*task-context port*; from then on each task reaches its own block, an
`AetherExecContext`, through the port, and the runtime makes, owns and
frees those blocks.

```c
typedef struct {
    AetherExecContext* (*current)(void);          /* the calling task's, or NULL */
    int  (*set_current)(AetherExecContext* ctx);  /* 0, or -1 */
    void (*relax)(void);                          /* may be NULL */
} AetherContextPort;

int  aether_context_port_install(const AetherContextPort* port);
int  aether_context_attach(void);
int  aether_context_detach(void);
void aether_context_task_ended(AetherExecContext* ctx);
int  aether_freertos_contexts_install(void);      /* ESP-IDF only */
```

`include/libaether.h` has the full contract; in short:

- **Install** once, on the task that owns the actors, before any other task
  enters Aether. That task keeps the built-in context. `-1` on a threaded
  runtime, a second install, or a port without `current`/`set_current`.
- **Attach** on each other task before its first call into Aether: the
  runtime makes a zeroed context and the port stores it. `-1` when the task
  has one, no port is installed, or there is no memory.
- **Detach** before the task ends, outside any call: the account it has in
  use is let go (as `aether_exec_use(NULL, NULL)`), its caller info, buffers
  and context-local blocks are freed, and its cancel cell goes back to a
  free list. `-1` when it has none, holds the built-in one, or is inside a
  call.
- **Task ended attached:** the port's deletion hook calls
  `aether_context_task_ended(ctx)`, from whichever task FreeRTOS cleans up
  on (the deleting task, or the idle task for a task that deleted itself),
  and the context is let go the same way. The built-in context is never
  freed.
- **Unattached task:** once a port is installed, a task that reaches the
  execution state with no context stops the program with
  `aether: a task with no execution context entered the runtime; call
  aether_context_attach() on each task before it calls into Aether`. It is
  never silently given another task's context.

**The ESP-IDF port** (`runtime/aether_context_freertos.c`, in the
cooperative source list, empty off ESP-IDF) keeps the context in FreeRTOS
thread-local pointer slot `AETHER_FREERTOS_TLS_SLOT` (1; ESP-IDF reserves
0 for pthreads) with `vTaskSetThreadLocalStoragePointerAndDelCallback`, and
relaxes with `vTaskDelay(1)`. The project needs
`CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS=2` (or more) and
`CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS=y` (the default); without them the
file still builds, so a single-task project needs no change, and
`aether_freertos_contexts_install()` returns -1 and says what to set.

**Cost.** With no port installed the accessor is one load and a predicted
branch, the built-in block as before. With a port, one call to `current`
per entry into the execution state (a `pvTaskGetThreadLocalStoragePointer`
on ESP-IDF). A task that never enters Aether pays its pointer slot, which
every FreeRTOS task has anyway, and nothing on its stack: the images the
ESP32 lane builds hold no thread-local section from Aether (the only
thread-locals are the C library's own 16 bytes: errno and the three stdio
pointers). Each attached task costs one heap block, 6,264 bytes on the
ESP32 and ESP32-S3 and 13,816 on the ESP32-C3 (whose `jmp_buf` is larger):
32 try and execution frames, their records and the 1 KiB outcome text; the
caller info (4 KiB) is added only once the task sets a caller. The built-in
context, the same size, is what a cooperative program already had in
`.bss`. A threaded build's code paths are those of before: its block is the
same thread-local, its allocation and send fast paths are untouched, and its
generated C preprocesses to the same text but for one added prototype.

## What each kind of state does per context

| State | Where | Per context, or shared and why |
|---|---|---|
| try and execution frames, their records, the frame floor | `AetherFrames.frames`, `.exec` | per context: a panic, deadline or cancel unwinds only its own task's calls |
| last outcome and its text | `.outcome`, `.outcome_text` | per context: `aether_last_outcome()` is the calling task's last call's |
| the allocation journal (owned locals, defers) | `.journal` | per context |
| account in use | `.account` | per context; an account is in use on one context at a time (`aether_exec_use` returns -1 for one in use on another), as on threads |
| ACTIVE-deadline timing | `.timed_*` | per context: time is charged to an account once, by the task that runs under it |
| the call deadline and its trip flag | `.deadline_at_ns`, `.deadline_tripped` | per context: `aether_set_call_deadline` is the calling task's |
| cancel cell and execution serials | `.cell`, `.serial` | per context; a detached context's cell is reused by the next attach, which counts serials above the old ones, so a stale token cancels nothing. Cancelling (`aether_exec_cancel`) is from any task |
| caller info (identity, attributes, deadline) | `.caller` | per context: one task's identity is never another's |
| refusing callback's reason, fn-pointer ownership mark, teardown queue, allocation test hook | `.callback_reason`, `.fnptr_owned`, `.teardown_*`, `.alloc_fail_after` | per context |
| generated builder blocks' stack; std.json's last error; the panic's captured trace | context-local blocks (`aether_context_local`) | per context on a cooperative build; thread-locals on a threaded one |
| std's per-call results and caches: std.net's joined header, pinned clock, server certificate-refusal text and request-body read; std.fs's stat, statvfs, mount, block-device, read-failure, positional and binary read caches; std.os's and std.ipc's error texts; the proxy's cookie buffer and trace-id state; std.cryptography's digest and random buffers; std.regex's and std.yaml's errors and yaml's emit buffer; the zlib, brotli, zstd and lzf buffers | context-local blocks | per context on a cooperative build, a buffer freed with the context; thread-locals on a threaded one |
| the sandbox's levels, refusal flag and staged write; the audit reader's cursor | context-local blocks | per context: one task's level never decides another's call |
| the HTTP client's idle connections; std.net's address-family memo | `aether_http.c`, `aether_net.c` | shared by design (any task may reuse a connection another put back), behind the context lock (`aether_context_lock`: an atomic flag, a waiter relaxing through the port) |
| the actor-step flags (`g_aether_in_actor_step`, the current actor's id) | `aether_panic.c` | plain globals: only the actors' task runs steps |
| execution accounts, their table of blocks, the live list | `aether_resource_caps.c` | shared by design, behind the accounts' lock (an atomic flag; a waiter relaxes through the port) |
| process memory cap and the bytes counted | `g_mem`, `g_use_slots` | shared by design, atomics (one slot without threads) |
| `aether_caps_slow`, `aether_caps_armed`, the cancel cell list | atomics | shared by design: fast-path flags and the cells tokens point at |
| callback registry, host events and manifest | `aether_callbacks.c`, `aether_host.c` | shared, written at start-up, read after |
| the cooperative scheduler, actors, mailboxes, the current step and reply slot, supervision's link and capture state | `aether_scheduler_coop.c`, `aether_supervision.c`, `aether_send_buffer.c` | the actors' task only (below) |
| the heap | libc / ESP-IDF | shared, thread-safe in ESP-IDF |

## Scheduler and re-entrancy constraints

- **Actors belong to the installing task.** The cooperative scheduler is
  one set of queues with no locks; making it per-context would mean a
  scheduler per task and cross-task delivery, a redesign. Instead spawn,
  send, ask, the scheduler's poll and wait stop the program on any other
  task (`aether: a spawn on a task that does not own the actors; ...`).
  Code on an attached task may use everything else: strings, collections,
  JSON, accounts, deadlines, try/panic, caller info, host services.
- **One task per context, and no migration.** A context is made for the
  task that attaches and is only ever that task's.
- **No context switching from Aether.** The port and attach/detach are host
  C calls; Aether code (and a guest a host runs on it, such as RexxAE)
  cannot name, select or switch a context, so it cannot take another task's
  account or caller identity.
- **Detach outside calls, and do not delete a task inside one.** A task
  deleted while inside a call into Aether leaves that call's frames and held
  account behind; FreeRTOS cannot unwind it.
- **Nothing from an interrupt.** No runtime function is ISR-safe, these
  included. Signal a task from the ISR instead.
- **Priorities.** Tasks of different priorities may enter Aether; the only
  lock they can meet is the accounts' (and the memory-cap fold's), and a
  waiter relaxes through the port rather than spin, so a holder of lower
  priority runs.

## The supported pattern: a native network task beside an Aether task

The single-Aether-task architecture stays supported and needs no port. For
more than one task:

1. `app_main` (or whichever task will own the actors) calls
   `aether_freertos_contexts_install()` first, then starts the other tasks.
2. A task that runs Aether code (a pure-Aether HTTP/TLS service, a RexxAE
   worker) calls `aether_context_attach()` at its start, makes its own
   execution account and puts it in use, sets its own caller, and calls
   `aether_context_detach()` before it deletes itself.
3. A native task that never calls into Aether (lwIP, the Wi-Fi event loop,
   an esp_http_server handler that only queues work) needs nothing. It may
   cancel another task's call with a token that task handed it
   (`aether_exec_cancel` touches only the cancel cell).
4. Work that needs actors is handed to the owning task (a FreeRTOS queue
   the owner drains between its own calls).

`tests/esp32/contexts` is this pattern, run under QEMU for the ESP32, the
ESP32-S3 and the ESP32-C3 (`sh tests/esp32/test_esp32_contexts.sh`).

## Not done, and why

- **Per-task actors.** Above: a scheduler per context is a redesign, not
  part of this.
- **std.net on ESP-IDF.** std.net does not build for ESP-IDF with
  networking on (no `poll.h`; lwIP's `tcp_close` clashes with std.tcp's; no
  `socketpair`), so an ESP-IDF component builds it with
  `AETHER_NO_NETWORKING`, as stubs. Its state above is per context on every
  cooperative build that has networking (the host test builds one); the
  ESP lane checks only that its images hold no thread-local section of
  Aether's.

## Keeping it so

No file of `std/` or `runtime/` that a cooperative build compiles may keep a
thread-local (`_Thread_local`, `__thread`, or `AETHER_TLS`, which is empty
there and so one global for every task) for state that belongs to a call.
Use a context-local block under `AETHER_NO_THREADING` and keep the
thread-local otherwise: a small struct and `aether_context_local` as
std.json's error does, `AETHER_CONTEXT_CAPS_BUFFER` (aether_panic.h) for a
split accessor's result buffer, or a lock (`aether_context_lock`) for state
shared on purpose. `coop_task_contexts` reads every object of the
cooperative list (built with and without networking, and the sandbox) for
thread-local symbols, and `make ci-coop` reads the whole cooperative
`libaether.a` (`tests/integration/coop_task_contexts/check_no_tls.sh`); both
fail on one. The actor-step flags and the actors' own state (`current_core_id`,
the reply slot and step actor, supervision's links and captures, the send
buffer) stay `AETHER_TLS`, one global on a cooperative build, because only
the actors' task touches them; `runtime/memory/aether_arena_optimized.c`'s
arena pointer is the same but nothing calls that allocator.
