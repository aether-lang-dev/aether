# Reply: task-local Aether execution state for cooperative ESP-IDF hosts

2026-11-22. Answers `asks/esp-idf-cooperative-task-local-execution-state.md`.
Status: implemented, in one PR with the TLS asks (branch
`batch/nov22-esp32-psk-deadlines-task-contexts`, not yet merged or
released); verified on hosts and in QEMU, not yet on a board.

## What we found on 0.935.0

The downstream 0.883 observation holds on 0.935.0, and is worse than an
accounting leak. Under `AETHER_NO_THREADING` every per-thread piece of
execution state was a plain global, so two FreeRTOS tasks entering Aether
shared the try and execution frames, the deadline and its trip flag, the
account in use and the ACTIVE-deadline timing, the cancel cell and
serials, the last outcome, the caller info, the allocation journal and the
builder blocks' stack. `tests/integration/coop_task_contexts` reproduces it
on a host: the cooperative runtime as the ESP32 builds it, two POSIX
threads standing in for tasks, one driven through quota, deadline, cancel
and panic while the other makes ordinary calls. Built the old way (no
port), 29 of 30 runs on cachyos failed, whether the threads ran on all
cores or pinned to one (`taskset -c 0`, closest to one FreeRTOS core):
SIGSEGV and SIGBUS when one task's deadline or panic longjmp'd into the
other's stack, and caller identities crossed in 60 of 60 rounds. The Mac
showed the same (15 of 15 runs failed).

## What shipped

Each task that enters Aether gets an execution context of its own, which
the runtime makes, owns and frees; the host only says where a task's
context is kept. `include/libaether.h`:

```c
typedef struct AetherExecContext AetherExecContext;   /* opaque */
typedef struct {
    AetherExecContext* (*current)(void);          /* the calling task's, or NULL */
    int  (*set_current)(AetherExecContext* ctx);  /* 0, or -1 */
    void (*relax)(void);                          /* may be NULL */
} AetherContextPort;

int  aether_context_port_install(const AetherContextPort* port);
int  aether_context_attach(void);
int  aether_context_detach(void);
void aether_context_task_ended(AetherExecContext* ctx);
int  aether_freertos_contexts_install(void);      /* ESP_PLATFORM only */
```

The ESP-IDF port is in the runtime (`runtime/aether_context_freertos.c`,
now in `runtime/coop_runtime_sources.txt`, empty off ESP-IDF): FreeRTOS
thread-local pointer slot 1 (`AETHER_FREERTOS_TLS_SLOT`) with its deletion
callback, and `vTaskDelay(1)` to relax. It needs
`CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS=2` and
`CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS=y` (the default), and refuses to
compile without them.

The contract (`docs/design/task-execution-contexts.md` in full):

- Install once, on the task that owns the actors, before any other task
  enters Aether. That task keeps the built-in context; actors (spawn, send,
  ask, poll, wait) run only there, and any other task that reaches them
  stops the program with a message.
- Every other task calls `aether_context_attach()` before its first call
  into Aether and `aether_context_detach()` outside any call before it
  ends. Detach lets go of the account it has in use, frees its caller info
  and buffers, and gives its cancel cell to the next context, which counts
  serials above the old ones so a stale token cancels nothing.
- A task deleted while attached is ended by the FreeRTOS deletion callback.
  A task must not be deleted while inside a call into Aether.
- Once a port is installed, a task that reaches the execution state
  without a context stops the program; it is never given another task's.
- One task per context; no migration; nothing from an ISR.
- Per context: frames and unwinding, the last outcome, the account in use
  and its budgets and timing, allocation charging (to the account in use on
  that task), the call deadline, cancellation, caller info, the journal,
  the builder blocks' stack, std.json's last error, the panic's trace, and
  every other piece of std and runtime state below.
  Shared by design: the heap and the process cap, the accounts themselves
  (one task at a time each, behind the accounts' lock, whose waiters now
  relax through the port instead of spinning against a holder of lower
  priority on one core), cancel tokens, the callback registry, the actors.
- Aether code, and a guest such as RexxAE, cannot name, select or switch a
  context, so it cannot take another task's account or caller identity.
- Without a port nothing changes for today's single-task hosts. Threaded
  builds are unaffected.

The builder blocks' stack of generated C now lives in the task's context
under `AETHER_NO_THREADING`, through a new runtime entry point
(`aether_context_local`), so the runtime ABI revision is 6: regenerate the
C of every library with the new compiler (the firmware already does, from
the release it pins).

## Verified

| Where | What | Result |
|---|---|---|
| cachyos (gcc 16) | `coop_task_contexts`: two tasks (quota, deadline, cancel, panic against ordinary calls), 2000 attach/detach rounds, 200 threads ended attached, unattached task, actors off the owning task | pass |
| cachyos | the same under `-fsanitize=address,undefined` (LeakSanitizer included) and under `-fsanitize=thread`, gcc and clang | clean (ThreadSanitizer first found two shared writes, the counter's stripe hint and the panic trace; both now per task) |
| cachyos | Espressif QEMU, ESP-IDF 6.1: `tests/esp32/test_esp32_contexts.sh` for ESP32, ESP32-S3, ESP32-C3 (built with `AETHER_NO_NETWORKING` and `AETHER_NO_FILESYSTEM`, as the firmware is) | all three pass: 20 interrupted rounds against the worker's, 0 wrong; a native task beside them; 200 attach/detach and 50 tasks deleted attached leave the free heap where it was (0 bytes); no thread-local section from Aether in any image |
| cachyos, ae-x64, ghostbsd, Mac | the ordinary suites | see the branch's final report; no regression found that is not environmental |
| cachyos, Mac | before/after benchmark (allocation-heavy, ping-pong, an export call with a try) | no measurable difference (below) |

Context size: 6,264 bytes on ESP32 and ESP32-S3, 13,816 on ESP32-C3; plus
4 KiB once a task sets a caller.

## Not verified, and not done

- **A real board.** Everything above is emulated. For a board run (Paul's):
  `ESP32_KEEP=/tmp/out sh tests/esp32/test_esp32_contexts.sh esp32c3`
  builds the image; flash `tests/esp32/contexts` with
  `idf.py -C tests/esp32/contexts set-target esp32c3 build flash monitor`
  (with `/gen/ctxlib.c` generated as the script does) and look for the
  `RESULT` lines.
- **Per-task actors.** The cooperative scheduler stays the owning task's;
  a scheduler per context would be a redesign.
- **std.net on ESP-IDF itself.** std.net does not build for ESP-IDF
  with networking on: no `poll.h`, lwIP's `tcp_close` clashes with
  std.tcp's, and there is no `socketpair`, `SCM_RIGHTS` or termios (tried
  with the contexts project and ESP-IDF 6.1, 2026-11-22). So an ESP-IDF
  component builds it with `AETHER_NO_NETWORKING`, which turns std.net
  into stubs, and the 0.883 HTTPS probe's networking was the firmware's own.
  The std.net state below is therefore per task on every cooperative build
  that has networking (a hosted one, as the host test builds), and absent
  from the ESP images; the QEMU lane checks only that those images still
  have no thread-local section of Aether's.

## std.net and the rest of std: per task too (same PR)

The first version of this reply left std.net's raw thread-locals for the
TLS branch, which was changing those files. The two branches were merged
into one PR, and a sweep of `std/` and `runtime/` found more than std.net:
every thread-local a cooperative build compiled, and every `AETHER_TLS`
variable, which a cooperative build turns into one plain global that all
tasks share. Each is now a block of the calling task's context
(`aether_context_local`) under `AETHER_NO_THREADING`; a threaded build keeps
its thread-locals and the same code.

- Raw thread-locals, which ESP-IDF would have put on every task's stack:
  std.net's joined response header (`tls_joined`, 8 KiB), the HTTP server's
  certificate-refusal text (`why`) and request-body read (`g_rbr_*`);
  std.fs's stat, statvfs, mount, block-device, read-failure, positional-read
  and binary-read caches; std.os's and std.ipc's error texts (three); the
  proxy's cookie buffer and trace-id random state; std.cryptography's digest
  and random-bytes buffers; std.regex's and std.yaml's last errors and
  yaml's emit buffer; the zlib, brotli, zstd and lzf result buffers; the
  Linux sandboxed spawn's error text; and, on Windows, std.os's launch error.
- Plain globals shared by all tasks: std.net's pinned clock
  (`aether_http_pinned_ms`), and the sandbox's levels, refusal flag and
  staged write (`t_sbx`, `t_total`, `t_fs_refused`, `t_stage_*`) and the
  audit reader's cursor. Shared, the sandbox let one task's level refuse
  another task's calls, and one task's pop end another's level.
- Left shared, on purpose: the actor-step flags `g_aether_in_actor_step`
  and `g_aether_current_actor_id` (only the actors' task runs steps), and
  the memory counter's stripe hint (already one atomic slot without
  threads). The LD_PRELOAD sandbox library is never linked into a program.

A buffer a block owns is freed with the context (at detach, or when the task
ends attached); with no memory for a block, an error text falls back to one
shared slot (as std.json's does), a buffer or cache panics, and the sandbox
stops the program rather than decide a call against state that is not the
caller's.

**The connection pool stays one pool, now locked.** The HTTP client's idle
connections, and std.net's address-family memo, are shared on purpose: a
connection one task put back is the next request's to reuse, whichever task
makes it, and a pool per task would hold idle sockets per task. Without
threads their mutex was a no-op; under `AETHER_NO_THREADING` each now takes
the context lock (`aether_context_lock`, an atomic flag whose waiter pauses
and then relaxes through the port, as the accounts' lock does). Held only
for list work and the close of an expired entry.

**Tested** by `tests/integration/coop_task_contexts`, which now builds the
cooperative runtime a second time with networking, a filesystem and the
sandbox and runs three more two-task modes: one task holds std.net's clock
pinned while the other must read it running; one holds a sandbox level that
grants nothing while the other must see none and be refused nothing; two make
300 keep-alive requests each through the one pool to a local server echoing
each path, and each response must be the task's own with its joined
duplicate headers; and (`tcpstop`) a native thread that never attaches
cancels an attached task's std.tcp read blocked on a silent peer, then a
deadline ends the next: the TLS branch's deadline and cancel keep their
state on the socket and the handle, nothing per thread or global, so they
work across tasks. It also reads every object of both builds for
thread-local symbols, and `make ci-coop` reads the whole cooperative
`libaether.a`. On the old code: the clock and sandbox modes fail (1 and
2 wrong), the pool mode passes plainly but under ThreadSanitizer reports the
race in `http_pool_put` and ends with a corrupt idle count, and five objects
(std.net twice, std.fs, std.os, std.ipc) hold thread-locals. On the new code
all pass, under ASan/UBSan and TSan too.

## What the firmware changes to use it

In `firmware/esp-idf`: add `CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS=2`
to the sdkconfig defaults; call `aether_freertos_contexts_install()` first
in `app_main`; in each task that calls into Aether (the HTTPS service, the
RexxAE worker) call `aether_context_attach()` at its start and
`aether_context_detach()` before it deletes itself, and make and use that
task's own execution account there; keep actor work on the `app_main`
task. The CMake source manifest needs no change (the port is in it).
