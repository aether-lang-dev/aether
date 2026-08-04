/* std.worker — see aether_worker.h.
 *
 * A job is: run `work` on its own OS thread; on return, hand the job either to
 * the host's installed poster (which marshals it to the loop thread and calls
 * aether_worker_deliver there) or, if no poster is installed, onto a
 * mutex-guarded ready-list that aether_worker_drain() pops on the caller's
 * thread. Either way `done(result)` runs on a thread the CALLER controls, never
 * on the worker — that is the whole point.
 *
 * Closure envs are NOT freed here: per the closure-lifetime rules a captured
 * env is a bounded per-closure leak by design (retain-on-capture never
 * releases), the fail-safe direction (leak >> use-after-free). We own and free
 * only the WorkerJob wrapper. The `void*` result is likewise owned by the
 * `done` closure — we never touch it. */
#include "aether_worker.h"
#include "../../runtime/utils/aether_thread.h"

#include <stdio.h>   /* the wait() poster diagnostic */
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#if !defined(_WIN32)
#  include <unistd.h>
#endif

/* Threads are the whole point of this module — but the cooperative /
 * AETHER_NO_THREADING build has no scheduler threads and deliberately leaves
 * pthread_create unstubbed (aether_thread.h: "calling them on a threadless
 * platform is a logic error and should fail at link time"). So when threading
 * is off we do NOT spawn: `work` runs synchronously on the caller's thread at
 * launch, and its result is delivered through the normal loop path (poster or
 * drain). No parallelism — but the contract (work runs; done fires on the loop
 * thread) still holds, which is exactly the cooperative model. This mirrors the
 * h2 server's "sequential fallback" when its pool is unavailable. */
#if defined(AETHER_NO_THREADING)
#  define AETHER_WORKER_HAS_THREADS 0
#else
#  define AETHER_WORKER_HAS_THREADS 1
#endif

/* ---- job ---------------------------------------------------------------- */

typedef struct WorkerJob {
    AetherWorkerClosure work;
    AetherWorkerClosure done;
    void*               result;
    int                 detached;   /* no done, no post-back */
    int                 raw;        /* run work only: no delivery, caller owns env (aether_worker_submit) */
    struct WorkerJob*   next;       /* ready-list link (drain path) */
} WorkerJob;

/* ---- module state ------------------------------------------------------- */

/* g_lock guards the ready-list and the poster slot. It is initialised lazily:
 * the Win32 threading shim maps pthread_mutex_t to a CRITICAL_SECTION, which
 * has no static initialiser (PTHREAD_MUTEX_INITIALIZER is POSIX-only), so we
 * cannot init it at file scope. ensure_init() runs pthread_mutex_init exactly
 * once, gated by an atomic CAS; first entry is a plain call before any worker
 * thread exists, so there is effectively no contention. */
static pthread_mutex_t g_lock;
static atomic_int g_init_state = 0;   /* 0 = uninit, 1 = initialising, 2 = ready */

#if AETHER_WORKER_HAS_THREADS
static pthread_cond_t g_pend_cv;   /* signalled on enqueue / shutdown */
/* Signalled whenever a job finishes: either it lands on the ready-list
 * for delivery, or a detached/raw job retires without one. This is what
 * aether_worker_wait() blocks on, so waiting costs no polling. */
static pthread_cond_t g_done_cv;
#endif

static void ensure_init(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_init_state, &expected, 1)) {
        pthread_mutex_init(&g_lock, NULL);
#if AETHER_WORKER_HAS_THREADS
        pthread_cond_init(&g_pend_cv, NULL);
        pthread_cond_init(&g_done_cv, NULL);
#endif
        atomic_store(&g_init_state, 2);
        return;
    }
    /* Another thread is initialising (or already did): wait until ready. */
    while (atomic_load(&g_init_state) != 2) { /* spin — bounded, one-shot */ }
}

/* Ready-list of completed jobs awaiting aether_worker_drain() (poster-less
 * path). Singly-linked FIFO: head popped, tail pushed. */
static WorkerJob* g_ready_head = NULL;
static WorkerJob* g_ready_tail = NULL;

/* Host-installed "post to the loop thread" trampoline. fn == NULL means none. */
static AetherWorkerClosure g_poster = { NULL, NULL };

/* Jobs launched but not yet delivered. */
static atomic_int g_pending = 0;

#if AETHER_WORKER_HAS_THREADS
/* Bounded pool (#1205). run() submits here instead of spawning a thread per
 * job; run_detached() still spawns a fresh thread. All fields under g_lock;
 * g_pend_cv wakes a worker when a job is queued or on shutdown. Reuses the
 * WorkerJob.next link (a job is on the pending queue OR the ready-list, never
 * both). */
static WorkerJob* g_pend_head = NULL;
static WorkerJob* g_pend_tail = NULL;
static pthread_t* g_pool = NULL;
static int        g_pool_n = 0;        /* threads actually running */
static int        g_pool_want = 0;     /* configured size; 0 = derive from cores */
static int        g_pool_started = 0;
static int        g_pool_shutdown = 0;
#endif

/* ---- closure invocation ------------------------------------------------- */
/* The compiler emits closure calls as fn(env, args...); we recover the real
 * signature with a cast (the fn pointer is type-erased to void(*)(void)). */

static void* call_work(AetherWorkerClosure c) {
    return ((void* (*)(void*))c.fn)(c.env);
}

static void call_done(AetherWorkerClosure c, void* result) {
    ((void (*)(void*, void*))c.fn)(c.env, result);
}

static void call_poster(AetherWorkerClosure c, void* job) {
    ((void (*)(void*, void*))c.fn)(c.env, job);
}

/* ---- ready-list (under g_lock) ----------------------------------------- */

/* Caller holds g_lock. */
static void ready_push(WorkerJob* job) {
    job->next = NULL;
    if (g_ready_tail) g_ready_tail->next = job;
    else              g_ready_head = job;
    g_ready_tail = job;
#if AETHER_WORKER_HAS_THREADS
    pthread_cond_broadcast(&g_done_cv);
#endif
}

/* A job retired without reaching the ready-list (detached, or a raw
 * submit): the pending count dropped, so a waiter must re-evaluate. */
static void note_job_retired(void) {
#if AETHER_WORKER_HAS_THREADS
    pthread_mutex_lock(&g_lock);
    pthread_cond_broadcast(&g_done_cv);
    pthread_mutex_unlock(&g_lock);
#endif
}

static WorkerJob* ready_pop(void) {
    WorkerJob* job = g_ready_head;
    if (job) {
        g_ready_head = job->next;
        if (!g_ready_head) g_ready_tail = NULL;
        job->next = NULL;
    }
    return job;
}

/* ---- job execution ----------------------------------------------------- */

/* Reclaim a job's per-job closure environments. The env is heap-allocated by
 * the compiler for the closure's captures; because we received the closures
 * through an extern boundary, codegen does NOT free them (the env-drain is
 * suppressed, closure_extern_retains_no_uaf), so ownership is ours once the
 * closures have fired.
 *
 * Goes through the env's own destructor (#1398), so the references its string
 * captures own are released too. A plain free reclaimed the struct and leaked
 * every captured string, which is what `worker.run` capturing a path did.
 * The poster env is host-owned (installed once) and is never touched here. */
extern void aether_closure_env_free(void* env);

static void free_job_envs(WorkerJob* job) {
    if (job->work.env) aether_closure_env_free(job->work.env);
    if (job->done.env) aether_closure_env_free(job->done.env);
}

/* Run the work closure, then route the completion. Called on a worker thread
 * (threaded build) or inline on the caller's thread (AETHER_NO_THREADING). */
static void run_job(WorkerJob* job) {
    if (job->raw) {
        ((void (*)(void*))job->work.fn)(job->work.env);
        free(job);
        return;
    }

    job->result = call_work(job->work);

    if (job->detached) {
        /* No post-back, no delivery: the result is the work closure's own
         * business (it either returns something disposable or NULL). */
        free_job_envs(job);
        atomic_fetch_sub(&g_pending, 1);
        free(job);
        note_job_retired();
        return;
    }

    /* Snapshot the poster under the lock; if one is installed, hand the job to
     * it so the host can marshal to the loop thread. Otherwise queue for drain. */
    pthread_mutex_lock(&g_lock);
    AetherWorkerClosure poster = g_poster;
    int have_poster = (poster.fn != NULL);
    if (!have_poster) ready_push(job);
    pthread_mutex_unlock(&g_lock);

    if (have_poster) {
        call_poster(poster, job);   /* host marshals -> aether_worker_deliver */
    }
}

#if AETHER_WORKER_HAS_THREADS
static void* worker_entry(void* arg) {
    run_job((WorkerJob*)arg);
    return NULL;
}

/* ---- bounded pool (#1205) ---------------------------------------------- */

static int default_pool_size(void) {
#if defined(_WIN32)
    int n = 4;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 4;
#endif
    if (n < 2)  n = 2;
    if (n > 32) n = 32;
    return (int)n;
}

static void pend_push(WorkerJob* job) {   /* under g_lock */
    job->next = NULL;
    if (g_pend_tail) g_pend_tail->next = job;
    else             g_pend_head = job;
    g_pend_tail = job;
}
static WorkerJob* pend_pop(void) {        /* under g_lock */
    WorkerJob* job = g_pend_head;
    if (job) {
        g_pend_head = job->next;
        if (!g_pend_head) g_pend_tail = NULL;
        job->next = NULL;
    }
    return job;
}

static void* pool_worker(void* arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_pend_head && !g_pool_shutdown)
            pthread_cond_wait(&g_pend_cv, &g_lock);
        if (!g_pend_head && g_pool_shutdown) {
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
        WorkerJob* job = pend_pop();
        pthread_mutex_unlock(&g_lock);
        if (job) run_job(job);
    }
}

/* Caller holds g_lock. Returns the running worker count; 0 means the pool
 * could not start and the caller should fall back to a fresh thread. */
static int pool_ensure_started(void) {
    if (g_pool_started) return g_pool_n;
    g_pool_started = 1;
    int want = g_pool_want > 0 ? g_pool_want : default_pool_size();
    g_pool = (pthread_t*)calloc((size_t)want, sizeof(pthread_t));
    if (!g_pool) return 0;
    int made = 0;
    for (int i = 0; i < want; i++) {
        if (pthread_create(&g_pool[made], NULL, pool_worker, NULL) == 0) made++;
    }
    g_pool_n = made;
    if (made == 0) { free(g_pool); g_pool = NULL; }
    return made;
}

/* Caller must NOT hold g_lock. Returns 1 if queued, 0 if the pool is
 * unavailable. */
static int pool_submit(WorkerJob* job) {
    pthread_mutex_lock(&g_lock);
    int n = pool_ensure_started();
    if (n == 0) { pthread_mutex_unlock(&g_lock); return 0; }
    pend_push(job);
    pthread_cond_signal(&g_pend_cv);
    pthread_mutex_unlock(&g_lock);
    return 1;
}
#endif

/* ---- launch ------------------------------------------------------------- */

static int launch(AetherWorkerClosure work, AetherWorkerClosure done, int detached) {
    if (!work.fn) return 0;
    ensure_init();   /* runs on the caller's thread, before the worker starts */

    WorkerJob* job = (WorkerJob*)calloc(1, sizeof(WorkerJob));
    if (!job) return 0;
    job->work = work;
    job->done = done;
    job->detached = detached;

    atomic_fetch_add(&g_pending, 1);

#if AETHER_WORKER_HAS_THREADS
    /* run() submits to the bounded pool; run_detached() (and a pool that
     * failed to start) spawns a fresh self-reaping thread so a truly
     * long-blocking job never starves the pool. */
    if (!detached && pool_submit(job)) {
        return 1;
    }
    pthread_t th;
    if (pthread_create(&th, NULL, worker_entry, job) != 0) {
        atomic_fetch_sub(&g_pending, 1);
        free(job);
        return 0;
    }
    pthread_detach(th);   /* self-reaping; we never join */
#else
    /* Cooperative / threadless build: no off-thread execution. Run the work
     * synchronously here; the completion still flows through the poster/drain
     * path, so `done` fires on the loop thread on the next deliver/drain. */
    run_job(job);
#endif
    return 1;
}

int aether_worker_run(AetherWorkerClosure work, AetherWorkerClosure done) {
    return launch(work, done, 0);
}

int aether_worker_run_detached(AetherWorkerClosure work) {
    AetherWorkerClosure none = { NULL, NULL };
    return launch(work, none, 1);
}

/* Run `work.fn(work.env)` on the pool with no completion delivery and no env
 * ownership: the caller keeps and frees the env and does its own completion
 * (used by the h2 server, whose task carries per-session ready-queue + wake
 * plumbing). Returns 1 if it ran off-thread (pool or, if the pool can't
 * start, a fresh thread), 0 on a threadless build so the caller runs it
 * inline. */
int aether_worker_submit(AetherWorkerClosure work) {
    if (!work.fn) return 0;
    ensure_init();
#if AETHER_WORKER_HAS_THREADS
    WorkerJob* job = (WorkerJob*)calloc(1, sizeof(WorkerJob));
    if (!job) return 0;
    job->work = work;
    job->raw = 1;
    if (pool_submit(job)) return 1;
    pthread_t th;
    if (pthread_create(&th, NULL, worker_entry, job) == 0) {
        pthread_detach(th);
        return 1;
    }
    free(job);
    return 0;
#else
    return 0;
#endif
}

/* ---- poster / delivery / drain ----------------------------------------- */

void aether_worker_set_main_poster(AetherWorkerClosure poster) {
    ensure_init();
    pthread_mutex_lock(&g_lock);
    g_poster = poster;
    pthread_mutex_unlock(&g_lock);
}

void aether_worker_deliver(void* arg) {
    if (!arg) return;
    WorkerJob* job = (WorkerJob*)arg;
    if (job->done.fn) call_done(job->done, job->result);
    free_job_envs(job);   /* work + done envs are ours now that both have fired */
    atomic_fetch_sub(&g_pending, 1);
    free(job);
}

int aether_worker_drain(int max) {
    ensure_init();
    int ran = 0;
    for (;;) {
        if (max > 0 && ran >= max) break;
        pthread_mutex_lock(&g_lock);
        WorkerJob* job = ready_pop();
        pthread_mutex_unlock(&g_lock);
        if (!job) break;
        /* deliver() runs done + frees + decrements pending, all on this
         * (the caller's) thread. */
        aether_worker_deliver(job);
        ran++;
    }
    return ran;
}

int aether_worker_pending(void) {
    return atomic_load(&g_pending);
}

/* Block until every submitted job has completed AND had its completion
 * delivered, running those completions on THIS thread. The pool is left
 * running and reusable, so a reusable batch helper can call this once
 * per batch (pool_shutdown, the only previous way to join, tears down
 * the process-global pool and makes a second batch return nothing).
 *
 * Returns the number of completions delivered, or -1 when a main-thread
 * poster is installed: there, completions are routed to the host to
 * marshal onto the loop thread, so a wait on that same thread would
 * block the very thread that has to deliver them. Failing fast with a
 * diagnostic beats an unexplained hang; GUI hosts already have their
 * loop and do not need this call.
 *
 * No polling: the wait blocks on a condition variable signalled when a
 * job lands on the ready-list or retires detached. On a threadless
 * build, work already ran synchronously at submit time, so this reduces
 * to draining whatever queued. */
int aether_worker_wait(void) {
    ensure_init();

    pthread_mutex_lock(&g_lock);
    int have_poster = (g_poster.fn != NULL);
    pthread_mutex_unlock(&g_lock);
    if (have_poster) {
        fprintf(stderr,
                "aether: worker.wait() with a main-thread poster installed; "
                "completions are delivered by the host loop, not by wait(). "
                "Pump the host loop instead.\n");
        return -1;
    }

    int ran = 0;
    for (;;) {
        /* Deliver everything ready right now, on the caller's thread. */
        for (;;) {
            pthread_mutex_lock(&g_lock);
            WorkerJob* job = ready_pop();
            pthread_mutex_unlock(&g_lock);
            if (!job) break;
            aether_worker_deliver(job);
            ran++;
        }
        if (atomic_load(&g_pending) == 0) break;
#if AETHER_WORKER_HAS_THREADS
        /* Jobs are still in flight but nothing is deliverable yet. Block
         * until a worker signals. The pending re-check inside the lock
         * closes the race where the last job completes between the
         * drain above and the wait. */
        pthread_mutex_lock(&g_lock);
        if (!g_ready_head && atomic_load(&g_pending) > 0) {
            pthread_cond_wait(&g_done_cv, &g_lock);
        }
        pthread_mutex_unlock(&g_lock);
#else
        /* Threadless: work ran inline at submit, so a non-zero pending
         * with an empty ready-list cannot resolve itself. Bail rather
         * than spin. */
        break;
#endif
    }
    return ran;
}

/* ---- pool control (#1205) ---------------------------------------------- */

void aether_worker_pool_configure(int n) {
#if AETHER_WORKER_HAS_THREADS
    ensure_init();
    pthread_mutex_lock(&g_lock);
    if (!g_pool_started && n > 0) g_pool_want = n;
    pthread_mutex_unlock(&g_lock);
#else
    (void)n;
#endif
}

void aether_worker_pool_shutdown(void) {
#if AETHER_WORKER_HAS_THREADS
    pthread_mutex_lock(&g_lock);
    if (!g_pool_started || g_pool_n == 0) { pthread_mutex_unlock(&g_lock); return; }
    g_pool_shutdown = 1;
    pthread_cond_broadcast(&g_pend_cv);
    pthread_t* threads = g_pool;
    int n = g_pool_n;
    g_pool = NULL;
    g_pool_n = 0;
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < n; i++) pthread_join(threads[i], NULL);
    free(threads);
#endif
}
