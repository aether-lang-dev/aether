# Reply: task-local Aether execution state for cooperative ESP-IDF hosts

2026-11-22. Answers `asks/esp-idf-cooperative-task-local-execution-state.md`.
Status: implemented on branch `batch/esp-idf-task-context` (not yet merged
or released); verified on hosts and in QEMU, not yet on a board.

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
  the builder blocks' stack, std.json's last error, the panic's trace.
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
| cachyos | Espressif QEMU, ESP-IDF 6.1: `tests/esp32/test_esp32_contexts.sh` for ESP32, ESP32-S3, ESP32-C3 | all three pass: 20 interrupted rounds against the worker's, 0 wrong; a native task beside them; 200 attach/detach and 50 tasks deleted attached leave the free heap where it was (0 bytes); no thread-local section from Aether in any image |
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
- **std.net.** `std/net/aether_http.c` (`tls_joined`, 8 KiB of
  `_Thread_local`, and `aether_http_pinned_ms`) and
  `std/net/aether_http_server.c` (`why`, `g_rbr_*`) still use raw
  thread-locals, which ESP-IDF puts on every task's stack: this is very
  likely the IPC task's overflow in the 0.883 HTTPS probe. The HTTP client
  pool's mutex is a no-op without threads. Those files are being changed on
  another branch for the TLS asks; the fix is to make them context-local
  blocks under `AETHER_NO_THREADING`, as std.json's error now is.

## What the firmware changes to use it

In `firmware/esp-idf`: add `CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS=2`
to the sdkconfig defaults; call `aether_freertos_contexts_install()` first
in `app_main`; in each task that calls into Aether (the HTTPS service, the
RexxAE worker) call `aether_context_attach()` at its start and
`aether_context_detach()` before it deletes itself, and make and use that
task's own execution account there; keep actor work on the `app_main`
task. The CMake source manifest needs no change (the port is in it).
