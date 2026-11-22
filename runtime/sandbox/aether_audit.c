// aether_audit.c — sandbox audit trail for the in-process permission layer.
// See aether_audit.h for the contract.

#include "aether_audit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
/* The Win32-aware pthread shim (CRITICAL_SECTION on Windows) rather than raw
 * <pthread.h> — zig's MinGW target has no pthread.h. Portable across the
 * native POSIX, MSYS2, and zig-cross-Windows builds. */
#include "../utils/aether_thread.h"

// ---- Sink configuration (resolved once, lazily) ----

#define SINK_NONE   0
#define SINK_STDERR 1
#define SINK_FILE   2

static int  sink_mode = -1;          // -1 = not yet resolved
static FILE* sink_file = NULL;       // open log file in SINK_FILE mode

// ---- In-memory ring buffer ----
//
// Fixed capacity; once full, the oldest entry is overwritten. This
// bounds memory regardless of how long a sandboxed program runs. A
// program that needs every entry should drain via std.audit and
// audit.clear() periodically.

#define AUDIT_RING_CAP   256
#define AUDIT_CAT_MAX     64
#define AUDIT_RES_MAX    256

typedef struct {
    char cat[AUDIT_CAT_MAX];
    char res[AUDIT_RES_MAX];
    int  allowed;
} audit_entry;

static audit_entry ring[AUDIT_RING_CAP];
static int ring_head  = 0;   // index of next write
static int ring_count = 0;   // entries currently held (<= AUDIT_RING_CAP)

// Denials since the process started, under audit_lock like the ring and,
// like it, one count for the whole process. Never wraps with the ring and
// is not reset by aether_audit_clear.
static int64_t denied_total = 0;

/* Field accessors form one entry read in category/resource/allowed order.
 * Keep that read in thread-local storage so a concurrent append cannot move
 * the logical index between fields. This object is linked into executables
 * and emit-lib shared objects, so it needs the shared-library-safe TLS model. */
#define QUERY_NONE     0
#define QUERY_CATEGORY 1
#define QUERY_RESOURCE 2
#if defined(AETHER_NO_THREADING)
/* A cooperative build has no thread-locals (AETHER_TLS_SHARED is empty), so
 * with task contexts one task's read could move another's: the read is a
 * block of the calling task's execution context instead
 * (aether_context_local), and one shared only when there was no memory. */
#include "../actors/aether_panic.h"   /* aether_context_local */
typedef struct { audit_entry snapshot; int index_plus_one; int phase; } AuditQuery;
static const char audit_query_key = 0;
static AuditQuery audit_query_no_memory;
static AuditQuery* audit_query_here(void) {
    AuditQuery* q = (AuditQuery*)aether_context_local(&audit_query_key, sizeof(AuditQuery), NULL);
    return q ? q : &audit_query_no_memory;
}
#define query_snapshot (audit_query_here()->snapshot)
#define query_phase    (audit_query_here()->phase)   /* QUERY_NONE is 0 */
/* -1 as the thread-local starts: the block is zeroed, so it keeps the index plus one. */
#define query_index_get()  (audit_query_here()->index_plus_one - 1)
#define query_index_set(i) (audit_query_here()->index_plus_one = (i) + 1)
#else
static AETHER_TLS_SHARED audit_entry query_snapshot;
static AETHER_TLS_SHARED int query_index = -1;
static AETHER_TLS_SHARED int query_phase = QUERY_NONE;
#define query_index_get()  (query_index)
#define query_index_set(i) (query_index = (i))
#endif

// One mutex guards both the sink-mode resolution and the ring buffer.
// Audit is off the hot path of normal execution (it fires only inside
// a sandbox block), so a single coarse lock is fine.
//
// PTHREAD_MUTEX_INITIALIZER is POSIX-only: the Win32 shim maps
// pthread_mutex_t to a CRITICAL_SECTION, which has no static initialiser. On
// Windows, lazily pthread_mutex_init() the lock exactly once via an atomic
// gate (audit fires only inside a sandbox block, and the first such call runs
// before any concurrent one in practice). audit_lock_get() returns the ready
// lock on every platform.
#if defined(_WIN32)
#include <stdatomic.h>
static pthread_mutex_t audit_lock;
static atomic_int audit_lock_state = 0;   /* 0 uninit, 1 initing, 2 ready */
static pthread_mutex_t* audit_lock_get(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&audit_lock_state, &expected, 1)) {
        pthread_mutex_init(&audit_lock, NULL);
        atomic_store(&audit_lock_state, 2);
    } else {
        while (atomic_load(&audit_lock_state) != 2) { /* spin: one-shot */ }
    }
    return &audit_lock;
}
#else
static pthread_mutex_t audit_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t* audit_lock_get(void) { return &audit_lock; }
#endif

// Resolve the sink mode from AETHER_SANDBOX_AUDIT on first use. Caller
// must hold audit_lock.
static void resolve_sink_locked(void) {
    if (sink_mode != -1) return;
    const char* v = getenv("AETHER_SANDBOX_AUDIT");
    if (!v || strcmp(v, "none") == 0) {
        sink_mode = SINK_NONE;
    } else if (strcmp(v, "stderr") == 0) {
        sink_mode = SINK_STDERR;
    } else if (strcmp(v, "file") == 0) {
        sink_mode = SINK_FILE;
    } else {
        // Unknown value — treat as off rather than guessing.
        sink_mode = SINK_NONE;
    }
}

// Emit one entry to the live sink. Caller must hold audit_lock.
static void sink_emit_locked(const char* category, const char* resource,
                             int allowed) {
    if (sink_mode == SINK_NONE) return;
    // Denials keep the AETHER_DENIED: prefix the LD_PRELOAD layer and
    // existing tooling already grep for; allowed checks get a parallel
    // AETHER_ALLOWED: prefix.
    const char* tag = allowed ? "AETHER_ALLOWED" : "AETHER_DENIED";
    if (sink_mode == SINK_STDERR) {
        fprintf(stderr, "%s: %s %s\n", tag, category, resource);
    } else if (sink_mode == SINK_FILE) {
        if (!sink_file) {
            sink_file = fopen("aether-sandbox.log", "a");
        }
        if (sink_file) {
            fprintf(sink_file, "%s: %s %s\n", tag, category, resource);
            fflush(sink_file);
        }
    }
}

void aether_sandbox_audit(const char* category, const char* resource,
                          int allowed) {
    if (!category) category = "";
    if (!resource) resource = "";

    pthread_mutex_lock(audit_lock_get());

    resolve_sink_locked();
    sink_emit_locked(category, resource, allowed);

    // Append to the ring buffer unconditionally — the query API is
    // always available even when the live sink is "none". The buffer is
    // small and bounded, so this is cheap.
    audit_entry* e = &ring[ring_head];
    snprintf(e->cat, sizeof(e->cat), "%s", category);
    snprintf(e->res, sizeof(e->res), "%s", resource);
    e->allowed = allowed ? 1 : 0;
    ring_head = (ring_head + 1) % AUDIT_RING_CAP;
    if (ring_count < AUDIT_RING_CAP) ring_count++;
    if (!allowed) denied_total++;

    pthread_mutex_unlock(audit_lock_get());
}

// ---- Query API ----
//
// Entries are reported oldest-first. With a wrapped ring, the oldest
// live entry sits at ring_head; index it modulo capacity.

static int logical_to_physical(int i) {
    if (ring_count < AUDIT_RING_CAP) return i;          // not wrapped yet
    return (ring_head + i) % AUDIT_RING_CAP;            // wrapped
}

// Copy one logical entry while the caller holds audit_lock.
static void snapshot_entry_locked(int i) {
    memset(&query_snapshot, 0, sizeof(query_snapshot));
    query_snapshot.allowed = -1;
    if (i >= 0 && i < ring_count) {
        query_snapshot = ring[logical_to_physical(i)];
    }
    query_index_set(i);
}

int aether_audit_count(void) {
    pthread_mutex_lock(audit_lock_get());
    int n = ring_count;
    pthread_mutex_unlock(audit_lock_get());
    return n;
}

const char* aether_audit_entry_category(int i) {
    pthread_mutex_lock(audit_lock_get());
    snapshot_entry_locked(i);
    pthread_mutex_unlock(audit_lock_get());
    query_phase = QUERY_CATEGORY;
    return query_snapshot.cat;
}

const char* aether_audit_entry_resource(int i) {
    if (query_phase != QUERY_CATEGORY || query_index_get() != i) {
        pthread_mutex_lock(audit_lock_get());
        snapshot_entry_locked(i);
        pthread_mutex_unlock(audit_lock_get());
    }
    query_phase = QUERY_RESOURCE;
    return query_snapshot.res;
}

int aether_audit_entry_allowed(int i) {
    if ((query_phase != QUERY_CATEGORY && query_phase != QUERY_RESOURCE) ||
        query_index_get() != i) {
        pthread_mutex_lock(audit_lock_get());
        snapshot_entry_locked(i);
        pthread_mutex_unlock(audit_lock_get());
    }
    int allowed = query_snapshot.allowed;
    query_phase = QUERY_NONE;
    return allowed;
}

int64_t aether_audit_denied_total(void) {
    pthread_mutex_lock(audit_lock_get());
    int64_t n = denied_total;
    pthread_mutex_unlock(audit_lock_get());
    return n;
}

void aether_audit_clear(void) {
    pthread_mutex_lock(audit_lock_get());
    ring_head = 0;
    ring_count = 0;
    pthread_mutex_unlock(audit_lock_get());
}
