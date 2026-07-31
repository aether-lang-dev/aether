/* std.worker — off-thread worker + completion-on-the-loop-thread.
 *
 * The primitive every GUI toolkit has (Qt QThread+signal, GTK g_thread +
 * g_idle_add, Swing SwingWorker): run a closure on a background thread; when it
 * returns, deliver its result to a completion closure back on the thread that
 * owns the app's event loop, so the completion may safely touch UI / shared
 * state without a data race.
 *
 * This layer is toolkit-agnostic. It knows how to run work off-thread and how
 * to hold the completion until someone delivers it; it does NOT know how to get
 * onto "the main thread" — that is the host's job (aether-ui installs a poster
 * wrapping g_idle_add / dispatch_async / PostMessage). With no poster installed,
 * completed jobs queue on a pollable ready-list and aether_worker_drain() runs
 * them on the calling thread — the deterministic, toolkit-free path used by
 * headless apps and tests.
 *
 * Closures cross the FFI boundary as the compiler's `_AeClosure`
 * ({ fn, env }) by value; the env survives the hand-off because an extern
 * callee is treated as escaping (see the closure_extern_retains_no_uaf
 * regression). We invoke them with the emitted calling convention
 * fn(env, args...). */
#ifndef AETHER_WORKER_H
#define AETHER_WORKER_H

/* Mirror of the compiler-emitted _AeClosure. Two type-erased fields: a
 * function pointer and the captured-environment pointer. Passed BY VALUE. */
typedef struct { void (*fn)(void); void* env; } AetherWorkerClosure;

/* Run `work` on a pooled/background thread. When it returns a `void*` result,
 * `done` is invoked with that result — via the installed main-thread poster if
 * one exists, else queued for aether_worker_drain(). Returns 1 if the worker
 * was launched, 0 on failure (no threads available / allocation failure); on 0
 * neither closure runs. */
int aether_worker_run(AetherWorkerClosure work, AetherWorkerClosure done);

/* Fire-and-forget: run `work` off-thread and discard its result. No post-back,
 * nothing to drain. Returns 1 if launched, 0 on failure. */
int aether_worker_run_detached(AetherWorkerClosure work);

/* Run `work` on the shared pool with no completion delivery and no env
 * ownership (the caller keeps/frees the env and handles its own completion).
 * The single entry point for library subsystems that need pooled blocking
 * work but deliver results their own way. Returns 1 if it ran off-thread, 0
 * on a threadless build (caller runs it inline). */
int aether_worker_submit(AetherWorkerClosure work);

/* Install the host's "post to the loop thread" trampoline. `poster` is invoked
 * (on the worker thread) with a single opaque job pointer; the host must
 * marshal that pointer onto its loop thread and there call
 * aether_worker_deliver(job). Passing a null-fn closure clears the poster
 * (reverting to the drain path). */
void aether_worker_set_main_poster(AetherWorkerClosure poster);

/* Run a job's completion NOW, on the calling thread: invokes done(result) and
 * frees the job. The host calls this from its loop thread (from inside the
 * poster's marshaled callback); aether_worker_drain() calls it internally.
 * A null job is a no-op. */
void aether_worker_deliver(void* job);

/* Pump up to `max` ready completions on the calling thread (0 or negative =
 * all currently ready). Returns how many ran. Only jobs that finished while NO
 * poster was installed land here; with a poster, completions flow through it
 * instead. This is the headless / test loop. */
int aether_worker_drain(int max);

/* Number of launched jobs not yet delivered (running, or ready-but-not-drained,
 * or in flight through the poster). For backpressure and test synchronisation. */
int aether_worker_pending(void);

/* Block until every submitted job has completed and been delivered,
 * running completions on the calling thread; leaves the pool reusable.
 * Returns the number of completions delivered, or -1 if a main-thread
 * poster is installed (there the host loop delivers, so waiting on that
 * thread would deadlock). No polling: blocks on a condition variable. */
int aether_worker_wait(void);

/* Set the bounded pool size. Only takes effect before the pool starts
 * (first run()); a no-op afterwards and on threadless builds. Default is
 * derived from the core count. */
void aether_worker_pool_configure(int n);

/* Join and free the pool threads once they finish their current and queued
 * jobs. For deterministic teardown (tests); idempotent. NOT called at process
 * exit: exit abandons in-flight/queued jobs, so a job blocked in user work
 * can never hang the process (as the pre-pool thread-per-job model behaved).
 * Only call this when the pool's jobs are known to complete. */
void aether_worker_pool_shutdown(void);

#endif /* AETHER_WORKER_H */
