// aether_sandbox.c: the grant levels each thread is inside, and the check.
//
// The levels used to live in the generated code of each program and
// library (a stack per translation unit), with one process-wide pointer to
// the checker an enforce installed and cleared (#2955). A thread leaving
// its enforce cleared the pointer while another thread was inside one, so
// that thread ran unchecked, or called the pointer after it was cleared and
// crashed (D1); and a library's export called from inside another's
// enforce checked against its own, empty stack, or replaced the outer
// checker and then cleared it (D2). Now each thread's levels are here,
// once per runtime image, and nothing is installed: the check reads the
// calling thread's levels, behind a count of threads that have any.

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE   /* RTLD_DEFAULT in glibc's <dlfcn.h> */
#endif
#include "aether_sandbox.h"

#ifdef AETHER_HAS_SANDBOX
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aether_sandbox_path.h"
#include "actors/aether_panic.h"
#include "aether_resource_caps.h"
#include "aether_rv_mark.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <io.h>            /* _get_osfhandle, _open_osfhandle: the checked opens */
#  include <fcntl.h>         /* _O_* */
#  include <sys/stat.h>      /* _S_IWRITE */
#else
#  include <dirent.h>        /* fdopendir: aether_sandbox_opendir */
#  include <time.h>          /* time: aether_sandbox_temp_in */
#endif
#if !defined(_WIN32) && (defined(__unix__) || defined(__APPLE__))
#  include <dlfcn.h>
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>
#  elif defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#    include <link.h>
#    define AETHER_SANDBOX_PHDR 1
#  endif
#endif

/* A grant list is a std.list of (category, pattern) pairs, each element a
 * plain C string or an Aether string (a helper that keeps its `string`
 * parameter stores its own copy). */
extern int list_size(void*);
extern void* list_get_raw(void*, int);
extern const char* aether_string_data(const void*);
extern void aether_sandbox_audit(const char* category, const char* resource, int allowed);

// ---------------------------------------------------------------------------
// Runtime images that share a thread's levels (#2955, D2)
// ---------------------------------------------------------------------------
//
// A library built without --shared-runtime carries its own copy of the
// runtime, and on macOS and Windows that copy stays apart from the
// program's: its levels are its own. Called from inside another copy's
// enforce block (through a host service), it cannot read that block's
// grants, so it must not allow what they would refuse. Every copy counts
// the levels each thread has in all of them in one place, the rendezvous of
// the first copy it finds in the process (the earliest loaded with a record
// of them, aether_rv_mark.h), so a copy sees when this thread is inside
// levels it cannot read, and refuses (aether_sandbox_check_slow). On one
// runtime image all of this
// is one copy, and the count is its own.

/* The rendezvous's layout (#2957): its version and its size come first, so
 * a copy reads nothing else of one laid out otherwise. Version 1 had no
 * size. */
#define AETHER_SANDBOX_RV_VERSION 2

typedef struct AetherSandboxRendezvous {
    uint32_t version;
    uint32_t size;                // sizeof(AetherSandboxRendezvous)
    atomic_int* threads;          // threads with a level in force, in any copy
    int (*levels)(int delta);     // this thread's levels in every copy: adds delta, returns the total
} AetherSandboxRendezvous;

static atomic_int g_threads;
#if defined(AETHER_NO_THREADING)
/* A cooperative build has no thread-locals: AETHER_TLS_SHARED is empty there,
 * which made this file's per-thread state one block for every task of a
 * runtime with task contexts, so one task's levels were checked against
 * another's calls. Each is a block of the calling task's execution context
 * instead (aether_context_local, docs/design/task-execution-contexts.md).
 * With no memory for one the program stops: the sandbox does not decide a
 * call against state that is not the caller's. */
static void* sandbox_task_block(const void* key, size_t size) {
    void* b = aether_context_local(key, size, NULL);
    if (!b) {
        fprintf(stderr, "aether: no memory for this task's sandbox state\n");
        abort();
    }
    return b;
}
static const char t_total_key = 0;
#define t_total (*(int*)sandbox_task_block(&t_total_key, sizeof(int)))
#else
static AETHER_TLS_SHARED int t_total;
#endif

/* A step of this copy looks at the levels' count from now on (the step bit
 * of its one word, aether_step_barrier.h): levels may be in force where its
 * steps can see them (#3419). Set for good, and only then: a program that
 * checks a getenv or a file read but enforces nothing keeps its plain
 * steps. */
static void sandbox_levels_may_exist(void) {
    if (!(atomic_load_explicit(&aether_caps_slow, memory_order_relaxed) & AETHER_CAPS_SLOW_SANDBOX))
        atomic_fetch_or_explicit(&aether_caps_slow, AETHER_CAPS_SLOW_SANDBOX, memory_order_release);
}

static const struct AetherSandboxRendezvous* rv(void);

static int rv_levels(int delta) {
    int before = t_total;
    int after = before + delta;
    if (after < 0) after = 0;
    t_total = after;
    /* Read only by this thread's own checks: another thread seeing a stale
     * count takes the slow path and finds nothing of its own, or skips a
     * level that is not its own. */
    if (before == 0 && after > 0) {
        if (atomic_fetch_add_explicit(&g_threads, 1, memory_order_relaxed) == 0) {
            /* No thread had a level: from now on this copy's steps look. Its
             * count and hooks are published first, for a level another copy
             * pushed through this one's count (this copy may have looked up
             * nothing yet). */
            (void)rv();
            sandbox_levels_may_exist();
        }
    } else if (before > 0 && after == 0) {
        atomic_fetch_sub_explicit(&g_threads, 1, memory_order_relaxed);
    }
    return after;
}

static const AetherSandboxRendezvous g_own_rv = {
    AETHER_SANDBOX_RV_VERSION, (uint32_t)sizeof(AetherSandboxRendezvous), &g_threads, rv_levels
};

/* This copy's, which its record names (aether_rv_mark.h, #3445): the other
 * copies find it by the record where records are read, and by name where
 * not (AETHER_RV_FN). */
AETHER_RV_FN const AetherSandboxRendezvous* aether_sandbox_rendezvous(void) {
    return &g_own_rv;
}

#if AETHER_RV_MARKS
AETHER_RV_MARK(g_rv_mark, AETHER_RV_KIND_SANDBOX, aether_sandbox_rendezvous);
#  define RV_SELF (&g_rv_mark)
#else
#  define RV_SELF NULL
#endif

static const AetherSandboxRendezvous* _Atomic g_rv = NULL;
/* aether_sandbox_in_force is aether_panic.c's, where the step barrier reads
 * it without linking this file (#3393). */

/* This copy's hooks (aether_panic.h's AetherSandboxHooks, #3438): how a
 * panic ends the levels pushed inside the frame it leaves (#2955), and a
 * step's part (aether_step_barrier.h, #3393), each on this copy's own
 * levels, reached through no exported name another copy could interpose.
 * Registered before any level of this copy can exist (every push looks for
 * the copies first), so that a program that never enforces does not link
 * this file for its panics or its steps. */
static void sandbox_unwind_hook(int frames);
static int sandbox_step_hide(int saved[2]);
static void sandbox_step_show(const int saved[2]);
static const AetherSandboxHooks g_hooks = { sandbox_unwind_hook, sandbox_step_hide, sandbox_step_show };

/* Set while this copy's hooks found no place (more than
 * AETHER_SANDBOX_HOOKS_MAX copies share one panic): a panic does not unwind
 * its levels nor a step put them aside, and a trusted call's barrier a
 * panic left would let the rest of a block skip that block's grants, so
 * while this thread has a level its checks refuse (decide). Each push tries
 * again, as an unloaded copy frees a place. */
static _Atomic int g_hooks_unserved = 0;

static void sandbox_register_hooks(void) {
    static _Atomic int said = 0;
    int full = aether_sandbox_hooks_add(&g_hooks) != 0;
    atomic_store_explicit(&g_hooks_unserved, full, memory_order_release);
    if (full && atomic_exchange_explicit(&said, 1, memory_order_relaxed) == 0)
        fprintf(stderr,
                "aether: sandbox: more than %d runtime copies with a sandbox share one panic: "
                "this copy refuses every check inside its enforce blocks until one leaves; "
                "build the libraries with --shared-runtime\n",
                AETHER_SANDBOX_HOOKS_MAX);
}

typedef const AetherSandboxRendezvous* (*rendezvous_fn)(void);

#define RV_SYMBOL "aether_sandbox_rendezvous"

/* The first loaded image with a record of the levels (aether_rendezvous_find,
 * in aether_resource_caps.c, which the execution accounts share). */
static const AetherSandboxRendezvous* find_rendezvous(void) {
    void* p = aether_rendezvous_find(RV_SYMBOL, AETHER_RV_KIND_SANDBOX, RV_SELF);
    if (!p) return NULL;
    rendezvous_fn f;
    memcpy(&f, &p, sizeof(f));
    return f();
}

/* What a step of this copy reads: the count of the threads with a level,
 * and the step bit when the count is another copy's, whose levels this
 * copy's steps may run inside (a thread in that copy's enforce block that
 * steps an actor of this one). */
static void sandbox_publish(const AetherSandboxRendezvous* r) {
    atomic_store_explicit(&aether_sandbox_in_force, r->threads, memory_order_release);
    if (r != &g_own_rv) sandbox_levels_may_exist();
}

static const AetherSandboxRendezvous* rv(void) {
    const AetherSandboxRendezvous* r = atomic_load_explicit(&g_rv, memory_order_acquire);
    if (r) return r;
    /* Everything a reader of g_rv needs is in place before g_rv is published
     * (#3419): a thread that sees g_rv set sees all of it. */
    sandbox_register_hooks();
    const AetherSandboxRendezvous* found = find_rendezvous();
    if (found && found != &g_own_rv &&
        (found->version != AETHER_SANDBOX_RV_VERSION ||
         found->size != (uint32_t)sizeof(AetherSandboxRendezvous))) {
        /* Another layout (another Aether): only its version and size were
         * read. This copy counts its own levels apart, as copies that never
         * found each other do, and says so once. */
        fprintf(stderr,
                "aether: sandbox: this runtime copy cannot share sandbox levels with the one "
                "loaded first (levels version %u, %u bytes, against %u, %u): the levels that "
                "copy enforces are not seen here; build the libraries with one Aether "
                "version, or with --shared-runtime\n",
                (unsigned)AETHER_SANDBOX_RV_VERSION, (unsigned)sizeof(AetherSandboxRendezvous),
                (unsigned)found->version, (unsigned)found->size);
        found = NULL;
    }
    if (!found) found = &g_own_rv;
    sandbox_publish(found);
    const AetherSandboxRendezvous* expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(&g_rv, &expected, found,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        /* Another thread published first, the rendezvous it found: this
         * copy's count is that one's. */
        sandbox_publish(expected);
        return expected;
    }
    return found;
}

/* Before this copy's first step (#3438): a step reads only its own copy's
 * word, and the SANDBOX bit was set only when this copy's own count first
 * held a level or its own rv() found another copy's. A copy that had looked
 * nothing up when another copy's count held levels stepped an actor plainly
 * inside them, and the actor's first check found levels it could not read
 * and refused. Every step needs this copy's scheduler, so scheduler_init
 * joins once. Only another copy's rendezvous of this layout is pinned here:
 * this copy's own, none, or another layout is left to the first push or
 * check, as before, so a copy loaded before the one that will own the count
 * still finds it then, and another layout is reported where it matters. */
static void sandbox_join_copies(void) {
    if (atomic_load_explicit(&g_rv, memory_order_acquire)) return;
    const AetherSandboxRendezvous* found = find_rendezvous();
    if (found && found != &g_own_rv && found->version == AETHER_SANDBOX_RV_VERSION &&
        found->size == (uint32_t)sizeof(AetherSandboxRendezvous))
        (void)rv();   /* pins and publishes it, and sets this copy's step bit */
}

extern AETHER_COPY_LOCAL void (*_Atomic aether_sandbox_join)(void);

#if AETHER_GCC_COMPAT
/* A copy unloaded gives up its place among the hooks a panic and a step
 * call, which may be another copy's (#3438). */
AETHER_DESTRUCTOR static void sandbox_remove_hooks(void) {
    aether_sandbox_hooks_remove(&g_hooks);
}

/* The pointer is this copy's own (AETHER_COPY_LOCAL): under ELF
 * interposition a later library's constructor, which runs whatever the
 * interposition, reached the first copy's, whose scheduler then joined
 * through this copy's rendezvous. The hooks a panic and a step call are a
 * list each copy adds its own to (aether_panic.h), not a pointer the last to
 * look set (#3438). Set from NULL, once. The runtime is built with GCC or
 * clang only (the MSVC check compiles its headers); a compiler without
 * constructors would leave the join to the first push or check. */
AETHER_CONSTRUCTOR static void sandbox_install_join(void) {
    void (*none)(void) = NULL;
    atomic_compare_exchange_strong_explicit(&aether_sandbox_join, &none, sandbox_join_copies,
                                            memory_order_release, memory_order_relaxed);
}
#endif

// ---------------------------------------------------------------------------
// This thread's levels
// ---------------------------------------------------------------------------

typedef struct {
    void* grants;        // borrowed from the pusher; NULL allows (or a barrier)
    const void* image;   // the enforce site's generated code, NULL for any other push
    int site;
    int skip_to;         // a trusted call's barrier: the level it resumes below; -1 otherwise
    int frame;           // the try frames live at the push (aether_try_depth)
} SandboxLevel;

typedef struct {
    SandboxLevel level[AETHER_SANDBOX_MAX_LEVELS];
    int depth;
    /* The levels below are the code's an actor step runs inside (#3393):
     * put aside for the step, which is checked against its own alone. */
    int floor;
    const void* pending_image;
    int pending_site;
} SandboxThread;

#if defined(AETHER_NO_THREADING)
static const char t_sbx_key = 0;
#define t_sbx (*(SandboxThread*)sandbox_task_block(&t_sbx_key, sizeof(SandboxThread)))
#else
static AETHER_TLS_SHARED SandboxThread t_sbx;
#endif

static void push_level(SandboxThread* t, void* grants, const void* image, int site, int skip_to) {
    const AetherSandboxRendezvous* r = rv();
    if (atomic_load_explicit(&g_hooks_unserved, memory_order_acquire)) sandbox_register_hooks();
    if (t->depth >= AETHER_SANDBOX_MAX_LEVELS) {
        /* Left out, the level's grants would not apply, and its pop would
         * take an outer level off instead (a widening, both ways). */
        aether_panic("sandbox: more than 64 nested levels (enforce blocks, trusted calls, host run_sandboxed calls) on one thread");
        return;
    }
    SandboxLevel* v = &t->level[t->depth];
    v->grants = grants;
    v->image = image;
    v->site = site;
    v->skip_to = skip_to;
    v->frame = aether_try_depth();
    t->depth++;
    r->levels(1);
}

void aether_sandbox_push(void* grants) {
    SandboxThread* t = &t_sbx;
    const void* image = t->pending_image;
    int site = t->pending_site;
    t->pending_image = NULL;
    t->pending_site = 0;
    push_level(t, grants, image, site, -1);
}

void aether_sandbox_pop(void) {
    SandboxThread* t = &t_sbx;
    if (t->depth <= t->floor) return;   /* none of its own: an enclosing step's are not its to take */
    t->depth--;
    rv()->levels(-1);
}

int aether_sandbox_depth(void) {
    return t_sbx.depth;
}

/* This copy's levels down to `depth`: what the exported restore, this copy's
 * unwind and its step's show do, through no name another copy could
 * interpose (#3438). */
static void restore_levels(SandboxThread* t, int depth) {
    if (depth < t->floor) depth = t->floor;
    if (depth >= t->depth) return;
    int left = t->depth - depth;
    t->depth = depth;
    rv()->levels(-left);
}

void aether_sandbox_restore(int depth) {
    restore_levels(&t_sbx, depth);
}

void aether_sandbox_tag_site(const void* image, int site) {
    SandboxThread* t = &t_sbx;
    t->pending_image = image;
    t->pending_site = site;
}

/* A trusted call runs with the authority of the code that wrote the
 * enforce: a barrier level whose skip_to names the level that enforce
 * pushed, which the check jumps from to below it, so the levels between
 * (the block's grants, and any enforce inside it) do not apply. Never
 * higher: the site is looked for only among the levels in force (not ones
 * an earlier barrier hides), and one found nowhere (a block that escaped
 * its enforce, a level a panic left) pushes nothing. A barrier only ever
 * adds a level above the ones it hides, so nothing pushed inside reaches
 * them. */
int aether_sandbox_trust_enter(const void* image, int site) {
    SandboxThread* t = &t_sbx;
    int saved = t->depth;
    if (!image) return saved;
    for (int l = t->depth - 1; l >= t->floor; l--) {
        if (t->level[l].skip_to >= 0) { l = t->level[l].skip_to; continue; }
        if (t->level[l].image == image && t->level[l].site == site) {
            push_level(t, NULL, NULL, 0, l);
            break;
        }
    }
    return saved;
}

static void sandbox_unwind_hook(int frames) {
    SandboxThread* t = &t_sbx;
    t->pending_image = NULL;
    t->pending_site = 0;
    int d = t->depth;
    while (d > t->floor && t->level[d - 1].frame >= frames) d--;
    restore_levels(t, d);
}

void aether_sandbox_unwind_to_frame(int frames) {
    sandbox_unwind_hook(frames);
}

/* A step begins (aether_step_barrier.h, #3393): the levels of the code it
 * runs inside, in this copy and in every other on this thread, are put
 * aside, so the step is checked as its actor is on a thread of its own,
 * against nothing but the levels it pushes itself. 0 when the thread had
 * none. A step that ends gives them back, its own levels gone first (its
 * pops, or its panic's unwind, took them; any left are taken here). */
static int sandbox_step_hide(int saved[2]) {
    SandboxThread* t = &t_sbx;
    const AetherSandboxRendezvous* r = rv();
    int total = r->levels(0);
    if (total == 0 && t->depth == t->floor) return 0;
    saved[0] = t->floor;
    saved[1] = total;
    t->floor = t->depth;
    if (total) r->levels(-total);
    return 1;
}

static void sandbox_step_show(const int saved[2]) {
    SandboxThread* t = &t_sbx;
    restore_levels(t, t->floor);
    t->floor = saved[0];
    const AetherSandboxRendezvous* r = rv();
    int now = r->levels(0);
    if (saved[1] != now) r->levels(saved[1] - now);
}

// ---------------------------------------------------------------------------
// The check
// ---------------------------------------------------------------------------

static int grants_allow(void* grants, const char* category, const char* resource) {
    if (!grants) return 1;
    int n = list_size(grants);
    if (n == 0) return 0;
    for (int i = 0; i + 1 < n; i += 2) {
        const void* cat_v = list_get_raw(grants, i);
        const void* pat_v = list_get_raw(grants, i + 1);
        if (!cat_v || !pat_v) continue;
        const char* cat = aether_string_data(cat_v);
        const char* pat = aether_string_data(pat_v);
        if (!cat || !pat) continue;
        if (cat[0] == '*' && pat[0] == '*') return 1;
        if (strcmp(cat, category) == 0 && aether_grant_match(category, pat, resource)) return 1;
    }
    return 0;
}

static atomic_int g_foreign_reported;

/* 1 allowed, 0 refused, -1 nothing in force on this thread. `audit` 0 asks
 * without recording the answer (what a hard link would widen to). */
static int decide_as(const char* category, const char* resource, int audit) {
    SandboxThread* t = &t_sbx;
    int total = rv()->levels(0);
    if (total == 0 && t->depth == t->floor) return -1;
    if (atomic_load_explicit(&g_hooks_unserved, memory_order_acquire)) {
        if (audit) aether_sandbox_audit(category, resource, 0);   /* unserved: refused more, never less */
        return 0;
    }
    if (total > t->depth - t->floor) {
        /* Levels of another copy of the runtime are in force on this
         * thread, and their grants cannot be read from here. */
        if (!atomic_exchange_explicit(&g_foreign_reported, 1, memory_order_relaxed))
            fprintf(stderr,
                    "aether: sandbox: this thread is inside an enforce block of another copy of "
                    "the runtime (a library built without --shared-runtime), whose grants this "
                    "copy cannot read; refusing %s %s. Build the libraries with --shared-runtime "
                    "to nest sandboxes across them. (reported once)\n",
                    category ? category : "", resource ? resource : "");
        if (audit) aether_sandbox_audit(category, resource, 0);
        return 0;
    }
    /* From the top down, a trusted call's barrier jumping over the levels
     * it hides. With none left in force the check is the one an unsandboxed
     * program makes: allowed, and not audited. */
    int allowed = 1;
    int in_force = 0;
    for (int l = t->depth - 1; l >= t->floor; l--) {
        if (t->level[l].skip_to >= 0) { l = t->level[l].skip_to; continue; }
        in_force = 1;
        if (!grants_allow(t->level[l].grants, category, resource)) { allowed = 0; break; }
    }
    if (!in_force) return 1;
    if (audit) aether_sandbox_audit(category, resource, allowed);
    return allowed;
}

static int decide(const char* category, const char* resource) {
    return decide_as(category, resource, 1);
}

// The check sees an fs path where it leads (aether_sandbox_path.h). A path
// that cannot be resolved is refused, not matched as written.
int aether_sandbox_check_slow(const char* category, const char* resource) {
    const AetherSandboxRendezvous* r = rv();
    if (atomic_load_explicit(r->threads, memory_order_relaxed) == 0) return 1;
    if (!category) category = "";
    if (resource && category[0] == 'f' && category[1] == 's' &&
        (category[2] == '\0' || category[2] == '_')) {
        if (r->levels(0) == 0 && t_sbx.depth == t_sbx.floor) return 1;
        char resolved[PATH_MAX];
        if (!aether_sandbox_canon_path(resource, resolved, sizeof(resolved))) return 0;
        return decide(category, resolved) != 0;
    }
    return decide(category, resource) != 0;
}

int aether_sandbox_levels_here(void) {
    int total = rv()->levels(0);
    int own = t_sbx.depth - t_sbx.floor;
    return total > own ? total : own;
}

int aether_sandbox_check_resolved(const char* category, const char* resource) {
    const AetherSandboxRendezvous* r = rv();
    if (atomic_load_explicit(r->threads, memory_order_relaxed) == 0) return -1;
    return decide(category ? category : "", resource);
}

// The shell a command string runs through (aether_sandbox_match.h), for
// std.sandbox's grant_shell. Windows reads COMSPEC each call, as the CRT does.
const char* aether_sandbox_shell_path(void) {
    return aether_sandbox_shell();
}

// A grant's pattern in the form the checks compare against: an fs pattern's
// fixed directory resolved (std.sandbox's grant_fs_* call this), anything
// else copied. Returns a malloc'd string the grant list owns.
char* aether_sandbox_grant_pattern(const char* category, const char* pattern) {
    if (!pattern) return NULL;
    if (category && category[0] == 'f' && category[1] == 's') {
        char resolved[PATH_MAX];
        if (aether_sandbox_canon_pattern(pattern, resolved, sizeof(resolved))) return strdup(resolved);
    }
    return strdup(pattern);
}

// ---------------------------------------------------------------------------
// Calls made on the path that was checked (#3751)
// ---------------------------------------------------------------------------
//
// aether_sandbox_check resolves a path and matches where it leads; a caller
// that then hands the path as written to libc has it resolved a second time,
// and anything swapped in between (a directory turned into a symlink by
// another thread or process) is followed by the second resolution only. The
// calls below resolve once, check that resolution, and make the call on it
// following no symlink (aether_sandbox_walk, aether_sandbox_anchor). std.fs
// makes its opens and the calls that change a directory through them.

/* Whether this thread's last checked call was refused by the sandbox,
 * for std to tell a refusal from the system's own EACCES. */
#if defined(AETHER_NO_THREADING)
static const char t_fs_refused_key = 0;
#define t_fs_refused (*(int*)sandbox_task_block(&t_fs_refused_key, sizeof(int)))
#else
static AETHER_TLS_SHARED int t_fs_refused;
#endif

int aether_sandbox_refused(void) {
    return t_fs_refused;
}

static void fs_refused(void) {
    t_fs_refused = 1;
    errno = EACCES;
}

/* A level in force on this thread, in this copy or another. Without one
 * the calls below are the plain libc call, as before. */
static int fs_levels_here(void) {
    t_fs_refused = 0;
    const AetherSandboxRendezvous* r = rv();
    if (atomic_load_explicit(r->threads, memory_order_relaxed) == 0) return 0;
    return r->levels(0) != 0 || t_sbx.depth != t_sbx.floor;
}

int aether_sandbox_check_entry(const char* category, const char* resource) {
    if (!fs_levels_here()) return 1;
    char resolved[PATH_MAX];
    if (!resource || !aether_sandbox_canon_entry(resource, resolved, sizeof resolved)) return 0;
    return decide(category ? category : "", resolved) != 0;
}

/* What an access (AETHER_ACCESS_READ, _WRITE) of `resolved` needs. */
static int fs_access_allowed(const char* resolved, int access) {
    if ((access & AETHER_ACCESS_READ) && decide("fs_read", resolved) == 0) return 0;
    if ((access & AETHER_ACCESS_WRITE) && decide("fs_write", resolved) == 0) return 0;
    return 1;
}

/* The entry `path` names, checked for fs_write: 1 with it in `resolved`. */
static int fs_entry_writable(const char* path, char resolved[PATH_MAX]) {
    if (!path || !aether_sandbox_canon_entry(path, resolved, PATH_MAX) ||
        decide("fs_write", resolved) == 0) {
        fs_refused();
        return 0;
    }
    return 1;
}

/* A hard link: fs_write on the new name, which reaches the file as the old
 * name does, so it may not be granted more of it than the old name is
 * (fs_read and fs_write on `target` for whichever the new name has), or a
 * link into a writable directory would make any file there writable. A link
 * is made of the name `target` (linkat links a symlink itself), so that is
 * what is checked. 1 with both resolutions in `rt` and `rl`. */
static int fs_link_allowed(const char* target, const char* linkpath,
                           char rt[PATH_MAX], char rl[PATH_MAX]) {
    if (!fs_entry_writable(linkpath, rl)) return 0;
    if (!target || !aether_sandbox_canon_entry(target, rt, PATH_MAX)) {
        fs_refused();
        return 0;
    }
    static const char* const cats[2] = { "fs_read", "fs_write" };
    for (int i = 0; i < 2; i++) {
        if (decide_as(cats[i], rl, 0) == 1 && decide(cats[i], rt) == 0) {
            fs_refused();
            return 0;
        }
    }
    return 1;
}

int aether_sandbox_check_link(const char* target, const char* linkpath) {
    if (!fs_levels_here()) return 1;
    char rt[PATH_MAX], rl[PATH_MAX];
    return fs_link_allowed(target, linkpath, rt, rl);
}

int aether_sandbox_granted(const char* category, const char* path) {
    if (!fs_levels_here()) return 1;
    char resolved[PATH_MAX];
    if (!category || !path || !aether_sandbox_canon_path(path, resolved, sizeof resolved)) return 0;
    return decide_as(category, resolved, 0) != 0;
}

#ifdef _WIN32

/* Windows has no descriptor-relative calls in its C library, but its
 * handles serve the same end (#3751). Each call below opens what it acts on
 * (or the directory it acts in) without changing it, checks the final path
 * of that handle (GetFinalPathNameByHandleW: every junction and symlink on
 * the way followed) against the grants, and only then acts, through the
 * handle: a truncation by SetEndOfFile, a removal by its disposition, a
 * rename or a hard link by NtSetInformationFile with the verified
 * directory as RootDirectory, a new directory or file by NtCreateFile
 * relative to it. A junction swapped in between the check and the open is
 * caught at the open, before anything was changed. */

#include <winternl.h>

#ifndef FileDispositionInfoEx
#define AETHER_FILE_DISPOSITION_INFO_EX_CLASS 21
#else
#define AETHER_FILE_DISPOSITION_INFO_EX_CLASS FileDispositionInfoEx
#endif
#ifndef FILE_DISPOSITION_FLAG_DELETE
#define FILE_DISPOSITION_FLAG_DELETE 0x1
#define FILE_DISPOSITION_FLAG_POSIX_SEMANTICS 0x2
#endif
#ifndef IO_REPARSE_TAG_SYMLINK
#define IO_REPARSE_TAG_SYMLINK 0xA000000CL
#endif
#ifndef STATUS_DELETE_PENDING
#define STATUS_DELETE_PENDING ((NTSTATUS)0xC0000056L)
#endif
#ifndef STATUS_OBJECT_NAME_COLLISION
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035L)
#endif

typedef NTSTATUS (NTAPI* aether_nt_create_file_fn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                                                   PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG,
                                                   ULONG, ULONG, PVOID, ULONG);
typedef NTSTATUS (NTAPI* aether_nt_set_info_fn)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, int);
typedef ULONG (NTAPI* aether_nt_status_fn)(NTSTATUS);

/* An ntdll function by name; NULL when absent (never on NT). */
static void* win_ntdll(const char* name) {
    HMODULE m = GetModuleHandleW(L"ntdll.dll");
    FARPROC p = m ? GetProcAddress(m, name) : NULL;
    void* out;
    memcpy(&out, &p, sizeof out);
    return out;
}

static int win_errno_of(DWORD e) {
    switch (e) {
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME: return ENOENT;
        case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION:
        case ERROR_CANNOT_MAKE: return EACCES;
        case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return EEXIST;
        case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
        case ERROR_NOT_SAME_DEVICE: return EXDEV;
        case ERROR_DIRECTORY: return ENOTDIR;
        case ERROR_DISK_FULL: return ENOSPC;
        case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return ENOMEM;
        default: return EIO;
    }
}

/* A failure with Windows error `e`: GetLastError and errno both say it. */
static int win_fail(DWORD e) {
    SetLastError(e);
    errno = win_errno_of(e);
    return -1;
}

static int win_fail_status(NTSTATUS st) {
    aether_nt_status_fn to_dos = (aether_nt_status_fn)win_ntdll("RtlNtStatusToDosError");
    return win_fail(to_dos ? to_dos(st) : ERROR_GEN_FAILURE);
}

/* UTF-8 to UTF-16 into `out` (AETHER_SANDBOX_WPATH units): 1, or 0. */
static int win_wide(const char* s, wchar_t* out) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, AETHER_SANDBOX_WPATH) > 0;
}

/* Is what `h` is open on the file the check saw? Its final path is the
 * resolution, or one the grants allow for `access` themselves (a name
 * created since the check, in the case the volume stores). */
static int win_handle_is(HANDLE h, const char* resolved, int access) {
    /* A device ("\\.\NUL", aether_sandbox_wdevice) has no final path; its
     * name alone says what it is, with no directory or link to swap. */
    if (strncmp(resolved, "\\\\.\\", 4) == 0) return GetFileType(h) == FILE_TYPE_CHAR;
    char fin[PATH_MAX];
    if (!aether_sandbox_final_of_handle(h, fin, sizeof fin)) return 0;
    if (strcmp(fin, resolved) == 0) return 1;
    return fs_access_allowed(fin, access);
}

/* Remove what `h` is open on (opened with DELETE): POSIX semantics where
 * the volume has them, the classic disposition otherwise. */
static int win_delete_by_handle(HANDLE h) {
    FILE_DISPOSITION_INFO_EX ex;
    ex.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
    if (SetFileInformationByHandle(h, (FILE_INFO_BY_HANDLE_CLASS)AETHER_FILE_DISPOSITION_INFO_EX_CLASS,
                                   &ex, sizeof ex))
        return 0;
    DWORD e = GetLastError();
    if (e != ERROR_INVALID_PARAMETER && e != ERROR_NOT_SUPPORTED && e != ERROR_INVALID_FUNCTION)
        return win_fail(e);
    FILE_DISPOSITION_INFO d;
    d.DeleteFile = TRUE;
    if (SetFileInformationByHandle(h, FileDispositionInfo, &d, sizeof d)) return 0;
    return win_fail(GetLastError());
}

/* `resolved` split at its last separator: its directory (a root keeps its
 * "\") into `dir` and its last name, as UTF-16, into `wname`. */
static int win_split(const char* resolved, char dir[PATH_MAX], wchar_t* wname) {
    const char* sep = strrchr(resolved, '\\');
    if (!sep || !sep[1]) return 0;
    size_t dl = (size_t)(sep - resolved);
    if (dl == 2 && resolved[1] == ':') dl = 3;   /* "C:\x" is in "C:\" */
    if (dl >= PATH_MAX) return 0;
    memcpy(dir, resolved, dl);
    dir[dl] = '\0';
    return win_wide(sep + 1, wname);
}

/* The directory `dir` (a resolution), opened as a root for relative calls
 * and verified: its final path is `dir`, or the entry `wname` under the
 * final path is granted `category` itself. INVALID_HANDLE_VALUE with errno
 * set (EACCES, refused) otherwise. */
static HANDLE win_open_dir_verified(const char* dir, const wchar_t* wname, const char* category) {
    /* `wname` NULL: the directory itself is what is checked. */
    wchar_t wdir[AETHER_SANDBOX_WPATH];
    if (!win_wide(dir, wdir)) { win_fail(ERROR_INVALID_NAME); return INVALID_HANDLE_VALUE; }
    HANDLE h = CreateFileW(wdir, FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) { win_fail(GetLastError()); return h; }
    char fin[PATH_MAX];
    int ok = aether_sandbox_final_of_handle(h, fin, sizeof fin);
    if (ok && strcmp(fin, dir) != 0 && !wname) {
        ok = decide(category, fin) != 0;
    } else if (ok && strcmp(fin, dir) != 0) {
        char name[PATH_MAX];
        char entry[PATH_MAX];
        size_t fl = strlen(fin);
        ok = WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, (int)sizeof name, NULL, NULL) > 0 &&
             snprintf(entry, sizeof entry, "%s%s%s", fin, (fl && fin[fl - 1] == '\\') ? "" : "\\",
                      name) < (int)sizeof entry &&
             decide(category, entry) != 0;
    }
    if (!ok) {
        CloseHandle(h);
        fs_refused();
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

/* NtCreateFile of `wname` relative to the directory `root`, shared as
 * `share` says. */
static NTSTATUS win_create_at_shared(HANDLE root, const wchar_t* wname, ACCESS_MASK access, ULONG attrs,
                                     ULONG share, ULONG disposition, ULONG options,
                                     PSECURITY_DESCRIPTOR sd, HANDLE* out) {
    aether_nt_create_file_fn nt_create = (aether_nt_create_file_fn)win_ntdll("NtCreateFile");
    if (!nt_create) return (NTSTATUS)0xC0000002L;   /* STATUS_NOT_IMPLEMENTED */
    UNICODE_STRING us;
    us.Buffer = (PWSTR)wname;
    us.Length = (USHORT)(wcslen(wname) * sizeof(wchar_t));
    us.MaximumLength = us.Length;
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, root, sd);
    IO_STATUS_BLOCK io;
    return nt_create(out, access | SYNCHRONIZE, &oa, &io, NULL, attrs, share, disposition,
                     options | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
}

/* NtCreateFile of `wname` relative to the directory `root`. */
static NTSTATUS win_create_at(HANDLE root, const wchar_t* wname, ACCESS_MASK access, ULONG attrs,
                              ULONG disposition, ULONG options, PSECURITY_DESCRIPTOR sd, HANDLE* out) {
    return win_create_at_shared(root, wname, access, attrs,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, disposition,
                                options, sd, out);
}

/* NtSetInformationFile of a rename or hard link of `h` to `wname` in the
 * directory `root`. */
static int win_move_or_link(HANDLE h, HANDLE root, const wchar_t* wname, int link, BOOLEAN replace) {
    aether_nt_set_info_fn set_info = (aether_nt_set_info_fn)win_ntdll("NtSetInformationFile");
    if (!set_info) return win_fail(ERROR_NOT_SUPPORTED);
    size_t nl = wcslen(wname) * sizeof(wchar_t);
    size_t size = offsetof(FILE_RENAME_INFORMATION, FileName) + nl + sizeof(wchar_t);
    FILE_RENAME_INFORMATION* ri = (FILE_RENAME_INFORMATION*)calloc(1, size);
    if (!ri) return win_fail(ERROR_NOT_ENOUGH_MEMORY);
    ri->ReplaceIfExists = replace;
    ri->RootDirectory = root;
    ri->FileNameLength = (ULONG)nl;
    memcpy(ri->FileName, wname, nl);
    IO_STATUS_BLOCK io;
    NTSTATUS st = set_info(h, &io, ri, (ULONG)size, link ? 11 /* FileLinkInformation */
                                                          : 10 /* FileRenameInformation */);
    free(ri);
    return st >= 0 ? 0 : win_fail_status(st);
}

/* Open the entry `path` names (not followed), checked for fs_write and
 * verified, with `access` (DELETE and the rest). `resolved` gets its
 * resolution. With no level in force on this thread it is opened as given,
 * nothing checked. */
static HANDLE win_open_entry(const char* path, DWORD access, char resolved[PATH_MAX]) {
    int levels = fs_levels_here();
    wchar_t wp[AETHER_SANDBOX_WPATH];
    if (levels) {
        if (!aether_sandbox_canon_entry(path, resolved, PATH_MAX) || decide("fs_write", resolved) == 0) {
            fs_refused();
            return INVALID_HANDLE_VALUE;
        }
        if (!win_wide(resolved, wp)) { win_fail(ERROR_INVALID_NAME); return INVALID_HANDLE_VALUE; }
    } else {
        if (!win_wide(path, wp)) { win_fail(ERROR_INVALID_NAME); return INVALID_HANDLE_VALUE; }
        resolved[0] = '\0';
    }
    HANDLE h = CreateFileW(wp, access | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) { win_fail(GetLastError()); return h; }
    if (levels && !win_handle_is(h, resolved, AETHER_ACCESS_WRITE)) {
        CloseHandle(h);
        fs_refused();
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

/* What `h` is: 1 a directory that is not a link, 2 a link to a directory
 * (any name surrogate: a symlink or a junction, both of which std's
 * fs.is_symlink counts as a link, and which an unlink removes, leaving what
 * it points at), 0 anything else; -1 when it cannot be asked. */
static int win_entry_kind(HANDLE h) {
    FILE_ATTRIBUTE_TAG_INFO ti;
    if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &ti, sizeof ti)) return -1;
    if (!(ti.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    if ((ti.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(ti.ReparseTag))
        return 2;
    return 1;
}

#define WIN_RM_FILE 0
#define WIN_RM_DIR  1
#define WIN_RM_ANY  2

static int win_remove(const char* path, int what) {
    if (!path) return win_fail(ERROR_INVALID_PARAMETER);
    char resolved[PATH_MAX];
    HANDLE h = win_open_entry(path, DELETE, resolved);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int kind = win_entry_kind(h);
    int rc;
    if (kind < 0) rc = win_fail(GetLastError());
    else if (what == WIN_RM_FILE && kind == 1) { rc = win_fail(ERROR_ACCESS_DENIED); errno = EISDIR; }
    else if (what == WIN_RM_DIR && kind == 0) rc = win_fail(ERROR_DIRECTORY);
    else rc = win_delete_by_handle(h);
    DWORD e = GetLastError();
    int en = errno;
    CloseHandle(h);
    SetLastError(e);
    errno = en;
    return rc;
}

int aether_sandbox_unlink(const char* path) { fs_levels_here(); return win_remove(path, WIN_RM_FILE); }
int aether_sandbox_rmdir(const char* path) { fs_levels_here(); return win_remove(path, WIN_RM_DIR); }
int aether_sandbox_remove(const char* path) { fs_levels_here(); return win_remove(path, WIN_RM_ANY); }

/* A new directory: fs_write on its name, made by NtCreateFile relative to
 * its verified directory, with `sd` (NULL: its folder's). */
int aether_sandbox_win_mkdir(const char* path, void* sd) {
    if (!path) return win_fail(ERROR_INVALID_PARAMETER);
    int levels = fs_levels_here();
    char resolved[PATH_MAX];
    if (levels) {
        if (!aether_sandbox_canon_entry(path, resolved, sizeof resolved) || decide("fs_write", resolved) == 0) {
            fs_refused();
            return -1;
        }
    } else {
        wchar_t wp[AETHER_SANDBOX_WPATH];
        if (!win_wide(path, wp)) return win_fail(ERROR_INVALID_NAME);
        SECURITY_ATTRIBUTES sa = { sizeof sa, sd, FALSE };
        return CreateDirectoryW(wp, sd ? &sa : NULL) ? 0 : win_fail(GetLastError());
    }
    char dir[PATH_MAX];
    wchar_t wname[AETHER_SANDBOX_WPATH];
    if (!win_split(resolved, dir, wname)) return win_fail(ERROR_INVALID_NAME);
    HANDLE root = win_open_dir_verified(dir, wname, "fs_write");
    if (root == INVALID_HANDLE_VALUE) return -1;
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = win_create_at(root, wname, FILE_LIST_DIRECTORY, FILE_ATTRIBUTE_NORMAL, FILE_CREATE,
                                FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT, sd, &h);
    CloseHandle(root);
    if (st < 0) return win_fail_status(st);
    CloseHandle(h);
    return 0;
}

int aether_sandbox_mkdir(const char* path, int mode) {
    (void)mode;
    return aether_sandbox_win_mkdir(path, NULL);
}

/* mkdir -p: the whole path checked once, then each directory opened (or
 * made) by NtCreateFile relative to the one before it, a reparse point
 * where the check saw a directory refused: the resolution has none. */
int aether_sandbox_mkdir_p(const char* path, int mode) {
    (void)mode;
    if (!path || !*path) return win_fail(ERROR_INVALID_PARAMETER);
    char resolved[PATH_MAX];
    if (!aether_sandbox_canon_path(path, resolved, sizeof resolved) ||
        (fs_levels_here() && decide("fs_write", resolved) == 0)) {
        fs_refused();
        return -1;
    }
    wchar_t w[AETHER_SANDBOX_WPATH];
    if (!win_wide(resolved, w)) return win_fail(ERROR_INVALID_NAME);
    size_t root = aether_sandbox_wroot(w);
    if (!root) return win_fail(ERROR_INVALID_NAME);
    wchar_t saved = w[root];
    w[root] = L'\0';
    HANDLE dir = CreateFileW(w, FILE_LIST_DIRECTORY | FILE_TRAVERSE | SYNCHRONIZE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS, NULL);
    w[root] = saved;
    if (dir == INVALID_HANDLE_VALUE) return win_fail(GetLastError());
    wchar_t* p = w + root;
    while (*p) {
        wchar_t* e = p;
        while (*e && *e != L'\\') e++;
        wchar_t keep = *e;
        *e = L'\0';
        HANDLE next = INVALID_HANDLE_VALUE;
        NTSTATUS st = win_create_at(dir, p, FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
                                    FILE_ATTRIBUTE_NORMAL, FILE_OPEN_IF,
                                    FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT, NULL, &next);
        *e = keep;
        CloseHandle(dir);
        if (st < 0) return win_fail_status(st);
        FILE_ATTRIBUTE_TAG_INFO ti;
        if (!GetFileInformationByHandleEx(next, FileAttributeTagInfo, &ti, sizeof ti) ||
            (ti.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            CloseHandle(next);
            fs_refused();
            return -1;
        }
        dir = next;
        p = *e ? e + 1 : e;
    }
    CloseHandle(dir);
    return 0;
}

/* A rename: both names checked for fs_write, `from` opened (not followed)
 * and verified, and moved by its handle into the verified directory of
 * `to`, replacing what is there as MoveFileExW(MOVEFILE_REPLACE_EXISTING)
 * does. A move across volumes fails with EXDEV, as that call's does. */
int aether_sandbox_rename(const char* from, const char* to) {
    if (!from || !to) return win_fail(ERROR_INVALID_PARAMETER);
    int levels = fs_levels_here();
    char rf[PATH_MAX], rt[PATH_MAX];
    if (levels && (!aether_sandbox_canon_entry(to, rt, sizeof rt) || decide("fs_write", rt) == 0)) {
        fs_refused();
        return -1;
    }
    HANDLE h = win_open_entry(from, DELETE, rf);
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (!levels && !aether_sandbox_canon_entry(to, rt, sizeof rt)) {
        CloseHandle(h);
        return win_fail(ERROR_PATH_NOT_FOUND);
    }
    char dir[PATH_MAX];
    wchar_t wname[AETHER_SANDBOX_WPATH];
    int rc;
    if (!win_split(rt, dir, wname)) {
        rc = win_fail(ERROR_INVALID_NAME);
    } else {
        HANDLE root = win_open_dir_verified(dir, wname, "fs_write");
        if (root == INVALID_HANDLE_VALUE) rc = -1;
        else {
            rc = win_move_or_link(h, root, wname, 0, TRUE);
            DWORD e = GetLastError();
            CloseHandle(root);
            SetLastError(e);
        }
    }
    DWORD e = GetLastError();
    int en = errno;
    CloseHandle(h);
    SetLastError(e);
    errno = en;
    return rc;
}

/* A hard link: the check of aether_sandbox_check_link, `target` opened
 * (not followed) and verified, and linked by its handle into the verified
 * directory of `linkpath`. */
int aether_sandbox_link(const char* target, const char* linkpath) {
    if (!target || !linkpath) return win_fail(ERROR_INVALID_PARAMETER);
    char rt[PATH_MAX], rl[PATH_MAX];
    int levels = fs_levels_here();
    if (levels && !fs_link_allowed(target, linkpath, rt, rl)) return -1;
    if (!levels && (!aether_sandbox_canon_entry(target, rt, sizeof rt) ||
                    !aether_sandbox_canon_entry(linkpath, rl, sizeof rl)))
        return win_fail(ERROR_PATH_NOT_FOUND);
    wchar_t wt[AETHER_SANDBOX_WPATH];
    if (!win_wide(rt, wt)) return win_fail(ERROR_INVALID_NAME);
    HANDLE h = CreateFileW(wt, FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return win_fail(GetLastError());
    int rc;
    char dir[PATH_MAX];
    wchar_t wname[AETHER_SANDBOX_WPATH];
    if (levels && !win_handle_is(h, rt, AETHER_ACCESS_READ | AETHER_ACCESS_WRITE)) {
        fs_refused();
        rc = -1;
    } else if (!win_split(rl, dir, wname)) {
        rc = win_fail(ERROR_INVALID_NAME);
    } else {
        HANDLE root = levels ? win_open_dir_verified(dir, wname, "fs_write") : INVALID_HANDLE_VALUE;
        if (!levels) {
            wchar_t wd[AETHER_SANDBOX_WPATH];
            if (win_wide(dir, wd))
                root = CreateFileW(wd, FILE_LIST_DIRECTORY | FILE_TRAVERSE | SYNCHRONIZE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                   OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
            if (root == INVALID_HANDLE_VALUE) win_fail(GetLastError());
        }
        if (root == INVALID_HANDLE_VALUE) rc = -1;
        else {
            rc = win_move_or_link(h, root, wname, 1, FALSE);
            DWORD e = GetLastError();
            CloseHandle(root);
            SetLastError(e);
        }
    }
    DWORD e = GetLastError();
    int en = errno;
    CloseHandle(h);
    SetLastError(e);
    errno = en;
    return rc;
}

/* The CRT mode an fopen() mode asks for, parsed for the open below. */
typedef struct {
    DWORD access;       /* GENERIC_READ / GENERIC_WRITE */
    DWORD disposition;  /* OPEN_EXISTING, OPEN_ALWAYS, CREATE_NEW */
    DWORD flags;        /* FILE_ATTRIBUTE_* and FILE_FLAG_* */
    int truncate;       /* "w": emptied once verified */
    int crt;            /* _O_* for _open_osfhandle */
} WinOpenMode;

static int win_parse_mode(const char* mode, WinOpenMode* m) {
    memset(m, 0, sizeof *m);
    m->flags = FILE_ATTRIBUTE_NORMAL;
    int plus = 0, text = 0, binary = 0;
    char kind = mode[0];
    if (kind != 'r' && kind != 'w' && kind != 'a') return 0;
    for (const char* c = mode + 1; *c && *c != ','; c++) {
        switch (*c) {
            case '+': plus = 1; break;
            case 'b': binary = 1; break;
            case 't': text = 1; break;
            case 'x': m->disposition = CREATE_NEW; break;
            case 'N': m->crt |= _O_NOINHERIT; break;
            case 'D': m->flags |= FILE_FLAG_DELETE_ON_CLOSE; m->access |= DELETE; break;
            case 'T': m->flags |= FILE_ATTRIBUTE_TEMPORARY; break;
            case 'S': m->flags |= FILE_FLAG_SEQUENTIAL_SCAN; break;
            case 'R': m->flags |= FILE_FLAG_RANDOM_ACCESS; break;
            default: break;
        }
    }
    if (kind == 'r') {
        m->access |= plus ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
        m->crt |= plus ? _O_RDWR : _O_RDONLY;
        m->disposition = OPEN_EXISTING;
    } else {
        m->access |= plus ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_WRITE;
        m->crt |= plus ? _O_RDWR : _O_WRONLY;
        if (!m->disposition) m->disposition = OPEN_ALWAYS;
        m->truncate = kind == 'w';
        if (kind == 'a') m->crt |= _O_APPEND;
    }
    m->crt |= binary ? _O_BINARY : (text ? _O_TEXT : _O_TEXT);
    return 1;
}

/* The FILE_FLAG_* bits of CreateFileW's flags, which ReOpenFile takes. */
#define WIN_FLAG_BITS 0xFFF00000u

/* `h`, opened with DELETE so a refused new file could go, opened again
 * without it, shared as fopen shares a file (read and write). A handle
 * that keeps DELETE makes every later open that does not share delete,
 * which is every fopen and _open, fail with a sharing violation for as
 * long as it is open. Two steps: the first reopen has to share delete with
 * `h` itself; once `h` is closed the second one shares read and write
 * only. `h` is closed; the handle returned is the one to use (`h` itself
 * when the system will not reopen it). */
static HANDLE win_drop_delete(HANDLE h, DWORD access, DWORD flags) {
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    HANDLE wide = ReOpenFile(h, access, share | FILE_SHARE_DELETE, flags & WIN_FLAG_BITS);
    if (wide == INVALID_HANDLE_VALUE) return h;
    CloseHandle(h);
    HANDLE tight = ReOpenFile(wide, access, share, flags & WIN_FLAG_BITS);
    if (tight == INVALID_HANDLE_VALUE) return wide;
    CloseHandle(wide);
    return tight;
}

/* A new file made by NtCreateFile relative to the verified directory of
 * `resolved` (FILE_CREATE: nothing that exists is opened), for when the
 * open that may create cannot have DELETE: made in the directory the check
 * saw, it never lands outside the grant, so there is nothing to remove. */
static HANDLE win_create_in_verified(const char* resolved, const WinOpenMode* m, int* collided) {
    *collided = 0;
    char dir[PATH_MAX];
    wchar_t wname[AETHER_SANDBOX_WPATH];
    if (!win_split(resolved, dir, wname)) { win_fail(ERROR_INVALID_NAME); return INVALID_HANDLE_VALUE; }
    HANDLE root = win_open_dir_verified(dir, wname, "fs_write");
    if (root == INVALID_HANDLE_VALUE) return root;
    ULONG options = FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT;
    if (m->flags & FILE_FLAG_DELETE_ON_CLOSE) options |= FILE_DELETE_ON_CLOSE;
    if (m->flags & FILE_FLAG_SEQUENTIAL_SCAN) options |= FILE_SEQUENTIAL_ONLY;
    if (m->flags & FILE_FLAG_RANDOM_ACCESS) options |= FILE_RANDOM_ACCESS;
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = win_create_at_shared(root, wname, m->access | FILE_READ_ATTRIBUTES,
                                       m->flags & 0xFFFFu, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       FILE_CREATE, options, NULL, &h);
    CloseHandle(root);
    if (st < 0) {
        *collided = st == STATUS_OBJECT_NAME_COLLISION;
        win_fail_status(st);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

/* Opened as an fopen or open of `path` would open it, but changing nothing
 * first: no CREATE_ALWAYS, no TRUNCATE_EXISTING. The final path of the
 * handle is checked, and only then is the file emptied through it, for "w"
 * and O_TRUNC. A file that exists is opened as it is, with no DELETE, so
 * it is shared as fopen shares it. Where the open may create, it asks for
 * DELETE too, so a file it created and then refused is deleted through the
 * handle (a junction swapped in after the check leaves nothing behind
 * outside the grant), and the handle kept is reopened without DELETE
 * (win_drop_delete). Where DELETE cannot be had, the new file is made in
 * the verified directory instead. INVALID_HANDLE_VALUE with errno set. */
static HANDLE win_open_checked(const char* path, const WinOpenMode* m, int access) {
    char resolved[PATH_MAX];
    int entry = m->disposition == CREATE_NEW;
    if (!(entry ? aether_sandbox_canon_entry(path, resolved, sizeof resolved)
                : aether_sandbox_canon_path(path, resolved, sizeof resolved)) ||
        !fs_access_allowed(resolved, access)) {
        fs_refused();
        return INVALID_HANDLE_VALUE;
    }
    wchar_t wp[AETHER_SANDBOX_WPATH];
    if (!win_wide(path, wp)) { win_fail(ERROR_INVALID_NAME); return INVALID_HANDLE_VALUE; }
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    DWORD want = m->access;
    HANDLE h = INVALID_HANDLE_VALUE;
    int created = 0;
    for (int attempt = 0; attempt < 2 && h == INVALID_HANDLE_VALUE; attempt++) {
        if (m->disposition != CREATE_NEW) {
            /* A file that is there: opened as it is, nothing created. */
            want = m->access;
            h = CreateFileW(wp, want, share, NULL, OPEN_EXISTING, m->flags, NULL);
            if (h != INVALID_HANDLE_VALUE) break;
            if (m->disposition == OPEN_EXISTING || GetLastError() != ERROR_FILE_NOT_FOUND) {
                win_fail(GetLastError());
                return INVALID_HANDLE_VALUE;
            }
        }
        /* It may be created: DELETE too, so a refused new file can go. */
        want = m->access | DELETE;
        h = CreateFileW(wp, want, share, NULL, m->disposition, m->flags, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            created = m->disposition == CREATE_NEW || GetLastError() != ERROR_ALREADY_EXISTS;
            break;
        }
        DWORD e = GetLastError();
        if (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION) {
            win_fail(e);
            return INVALID_HANDLE_VALUE;
        }
        /* No DELETE here: made in the verified directory, or, when a name
         * appeared there meanwhile, opened as it is on the next round. */
        int collided = 0;
        want = m->access;
        h = win_create_in_verified(resolved, m, &collided);
        if (h != INVALID_HANDLE_VALUE) {
            created = 1;
            break;
        }
        if (!collided || m->disposition == CREATE_NEW) return INVALID_HANDLE_VALUE;
    }
    if (h == INVALID_HANDLE_VALUE) return h;
    if (!win_handle_is(h, resolved, access)) {
        if (created && (want & DELETE)) (void)win_delete_by_handle(h);
        CloseHandle(h);
        fs_refused();
        return INVALID_HANDLE_VALUE;
    }
    if ((want & DELETE) && !(m->access & DELETE)) h = win_drop_delete(h, m->access, m->flags);
    /* Only a file has an end to set: a device (NUL) is opened as it is. */
    if (m->truncate && !created && GetFileType(h) == FILE_TYPE_DISK) {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        if (!SetFilePointerEx(h, zero, NULL, FILE_BEGIN) || !SetEndOfFile(h)) {
            DWORD e = GetLastError();
            CloseHandle(h);
            win_fail(e);
            return INVALID_HANDLE_VALUE;
        }
    }
    return h;
}

void* aether_sandbox_fopen(const char* path, const char* mode) {
    if (!path || !mode || !fs_levels_here()) return fopen(path, mode);
    WinOpenMode m;
    if (!win_parse_mode(mode, &m)) { errno = EINVAL; return NULL; }
    HANDLE h = win_open_checked(path, &m, aether_fopen_access(mode));
    if (h == INVALID_HANDLE_VALUE) return NULL;
    int fd = _open_osfhandle((intptr_t)h, m.crt);
    if (fd < 0) {
        CloseHandle(h);
        return NULL;
    }
    FILE* fp = _fdopen(fd, mode);
    if (!fp) {
        int e = errno;
        _close(fd);
        errno = e;
    }
    return fp;
}

/* The descriptor open std's other writers make (_O_CREAT|_O_EXCL, a lock
 * file): the same steps, a CRT descriptor as the result. `mode` is the
 * CRT's _S_IREAD/_S_IWRITE: a new file without _S_IWRITE is read-only. */
int aether_sandbox_open(const char* path, int flags, int mode) {
    if (!path) { errno = EFAULT; return -1; }
    if (!fs_levels_here()) return _open(path, flags, mode);
    WinOpenMode m;
    memset(&m, 0, sizeof m);
    m.flags = FILE_ATTRIBUTE_NORMAL;
    int acc = flags & (_O_RDONLY | _O_WRONLY | _O_RDWR);
    m.access = acc == _O_RDWR ? (GENERIC_READ | GENERIC_WRITE) : acc == _O_WRONLY ? GENERIC_WRITE
                                                                               : GENERIC_READ;
    if ((flags & _O_CREAT) && (flags & _O_EXCL)) m.disposition = CREATE_NEW;
    else if (flags & _O_CREAT) m.disposition = OPEN_ALWAYS;
    else m.disposition = OPEN_EXISTING;
    m.truncate = (flags & _O_TRUNC) != 0;
    if ((flags & _O_CREAT) && !(mode & _S_IWRITE)) m.flags = FILE_ATTRIBUTE_READONLY;
    m.crt = flags & (_O_RDONLY | _O_WRONLY | _O_RDWR | _O_APPEND | _O_BINARY | _O_TEXT | _O_NOINHERIT);
    int access = 0;
    if (acc == _O_RDONLY || acc == _O_RDWR) access |= AETHER_ACCESS_READ;
    if (acc != _O_RDONLY || (flags & (_O_CREAT | _O_APPEND | _O_TRUNC))) access |= AETHER_ACCESS_WRITE;
    HANDLE h = win_open_checked(path, &m, access);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int fd = _open_osfhandle((intptr_t)h, m.crt);
    if (fd < 0) CloseHandle(h);
    return fd;
}

/* What a lookup sees, through a handle opened and verified as the others:
 * not followed with `nofollow`. */
int aether_sandbox_stat(const char* path, int nofollow, AetherSandboxStat* out) {
    if (!path || !out) { errno = EINVAL; return -1; }
    memset(out, 0, sizeof *out);
    int levels = fs_levels_here();
    char resolved[PATH_MAX];
    wchar_t wp[AETHER_SANDBOX_WPATH];
    if (levels) {
        if (!(nofollow ? aether_sandbox_canon_entry(path, resolved, sizeof resolved)
                       : aether_sandbox_canon_path(path, resolved, sizeof resolved)) ||
            decide("fs_read", resolved) == 0) {
            fs_refused();
            return -1;
        }
    }
    if (!win_wide(path, wp)) return win_fail(ERROR_INVALID_NAME);
    HANDLE h = CreateFileW(wp, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | (nofollow ? FILE_FLAG_OPEN_REPARSE_POINT : 0), NULL);
    if (h == INVALID_HANDLE_VALUE) return win_fail(GetLastError());
    if (levels && !win_handle_is(h, resolved, AETHER_ACCESS_READ)) {
        CloseHandle(h);
        fs_refused();
        return -1;
    }
    FILE_ATTRIBUTE_TAG_INFO ti;
    BY_HANDLE_FILE_INFORMATION bi;
    int ok = GetFileInformationByHandleEx(h, FileAttributeTagInfo, &ti, sizeof ti) &&
             GetFileInformationByHandle(h, &bi);
    DWORD e = GetLastError();
    CloseHandle(h);
    if (!ok) return win_fail(e);
    /* A link not followed: a symlink or a junction, a name surrogate, as
     * std's fs.is_symlink counts one on Windows. */
    if ((ti.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(ti.ReparseTag) &&
        nofollow)
        out->kind = AETHER_SANDBOX_KIND_SYMLINK;
    else if (ti.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        out->kind = AETHER_SANDBOX_KIND_DIR;
    else
        out->kind = AETHER_SANDBOX_KIND_FILE;
    out->size = ((int64_t)bi.nFileSizeHigh << 32) | bi.nFileSizeLow;
    int64_t t = ((int64_t)bi.ftLastWriteTime.dwHighDateTime << 32) | bi.ftLastWriteTime.dwLowDateTime;
    out->mtime = (t - 116444736000000000LL) / 10000000LL;
    out->mode = (out->kind == AETHER_SANDBOX_KIND_DIR ? 0040755 : 0100644) &
                ~((bi.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0222 : 0);
    return 0;
}

/* A symlink std made by name (CreateSymbolicLinkW has no handle form): its
 * name opened (not followed) and verified; one a junction swapped in sent
 * elsewhere is removed again through the handle. 0 kept, -1 refused. */
int aether_sandbox_win_verify_entry(const char* path) {
    if (!path || !fs_levels_here()) return 0;
    char resolved[PATH_MAX];
    if (!aether_sandbox_canon_entry(path, resolved, sizeof resolved)) { fs_refused(); return -1; }
    /* The name as given: what CreateSymbolicLinkW made, wherever it went. */
    wchar_t wg[AETHER_SANDBOX_WPATH];
    if (!win_wide(path, wg)) return win_fail(ERROR_INVALID_NAME);
    HANDLE h = CreateFileW(wg, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return win_fail(GetLastError());
    int ok = win_handle_is(h, resolved, AETHER_ACCESS_WRITE);
    if (!ok) (void)win_delete_by_handle(h);
    CloseHandle(h);
    if (!ok) { fs_refused(); return -1; }
    return 0;
}

/* A name of the form std's fs.write_atomic gives its temporary. */
static void win_stage_name(const char* name, char* tmp, size_t cap) {
    static volatile LONG counter = 0;
    LONG n = InterlockedIncrement(&counter);
    snprintf(tmp, cap, "%s.tmp.%lu.%ld", name, (unsigned long)GetCurrentProcessId(), (long)n);
}

int aether_sandbox_stage_begin(const char* path, AetherSandboxStaged* s) {
    if (!path || !s) { errno = EINVAL; return -1; }
    memset(s, 0, sizeof *s);
    if (!fs_levels_here()) return -2;
    char resolved[PATH_MAX];
    if (!aether_sandbox_canon_entry(path, resolved, sizeof resolved) || decide("fs_write", resolved) == 0) {
        fs_refused();
        return -1;
    }
    char dir[PATH_MAX];
    wchar_t wname[AETHER_SANDBOX_WPATH];
    if (!win_split(resolved, dir, wname)) return win_fail(ERROR_INVALID_NAME);
    if (!WideCharToMultiByte(CP_UTF8, 0, wname, -1, s->name, (int)sizeof s->name, NULL, NULL))
        return win_fail(ERROR_FILENAME_EXCED_RANGE);
    HANDLE root = win_open_dir_verified(dir, wname, "fs_write");
    if (root == INVALID_HANDLE_VALUE) return -1;
    for (int attempt = 0; attempt < 16; attempt++) {
        win_stage_name(s->name, s->tmp, sizeof s->tmp);
        wchar_t wtmp[AETHER_SANDBOX_WPATH];
        if (!win_wide(s->tmp, wtmp)) break;
        HANDLE h = INVALID_HANDLE_VALUE;
        NTSTATUS st = win_create_at(root, wtmp, GENERIC_WRITE | DELETE, FILE_ATTRIBUTE_NORMAL, FILE_CREATE,
                                    FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT, NULL, &h);
        if (st == STATUS_OBJECT_NAME_COLLISION) continue;
        if (st < 0) { CloseHandle(root); return win_fail_status(st); }
        HANDLE keep = INVALID_HANDLE_VALUE;
        if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &keep, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            DWORD e = GetLastError();
            (void)win_delete_by_handle(h);
            CloseHandle(h);
            CloseHandle(root);
            return win_fail(e);
        }
        int fd = _open_osfhandle((intptr_t)h, _O_WRONLY | _O_BINARY);
        if (fd < 0) {
            (void)win_delete_by_handle(keep);
            CloseHandle(keep);
            CloseHandle(h);
            CloseHandle(root);
            return -1;
        }
        s->anchor = (long long)(intptr_t)root;
        s->held = (long long)(intptr_t)keep;
        return fd;
    }
    CloseHandle(root);
    return win_fail(ERROR_FILE_EXISTS);
}

int aether_sandbox_stage_commit(AetherSandboxStaged* s) {
    HANDLE root = (HANDLE)(intptr_t)s->anchor;
    HANDLE keep = (HANDLE)(intptr_t)s->held;
    wchar_t wname[AETHER_SANDBOX_WPATH];
    int rc = win_wide(s->name, wname) ? win_move_or_link(keep, root, wname, 0, TRUE)
                                      : win_fail(ERROR_INVALID_NAME);
    DWORD e = GetLastError();
    int en = errno;
    if (rc != 0) (void)win_delete_by_handle(keep);
    CloseHandle(keep);
    CloseHandle(root);
    SetLastError(e);
    errno = en;
    return rc;
}

void aether_sandbox_stage_abort(AetherSandboxStaged* s) {
    HANDLE root = (HANDLE)(intptr_t)s->anchor;
    HANDLE keep = (HANDLE)(intptr_t)s->held;
    (void)win_delete_by_handle(keep);
    CloseHandle(keep);
    CloseHandle(root);
}

/* Each call's names start from a place of their own: the process-wide count
 * of calls, as the POSIX aether_sandbox_temp_in mixes in g_stage_counter.
 * Seeded by the tick count alone, every call in the same 15 ms tick began
 * at the same name, so make_temp_dir's directory and the make_temp_file
 * after it, both prefixed "t", asked for one name, which a delete still
 * pending made unusable (#3817). */
static volatile LONG g_win_temp_calls = 0;

/* fs.make_temp_file and fs.make_temp_dir: a fresh name in `dir`, made by
 * NtCreateFile relative to `dir` opened and verified, named as
 * GetTempFileNameA names one: three letters of the prefix, four hex digits
 * and ".tmp". The path returned is `dir` as given and the name; a file
 * comes back closed. A name in use, or one whose entry is deleted but still
 * held open (STATUS_DELETE_PENDING, which no create can take until the last
 * handle closes), is passed over for the next. */
int aether_sandbox_temp_in(const char* dir, const char* prefix, int is_dir, char* out, size_t cap) {
    if (!dir || !prefix || !out) { errno = EINVAL; return -1; }
    if (!fs_levels_here()) return -2;
    char rdir[PATH_MAX];
    if (!aether_sandbox_canon_path(dir, rdir, sizeof rdir) || decide("fs_write", rdir) == 0) {
        fs_refused();
        return -1;
    }
    HANDLE root = win_open_dir_verified(rdir, NULL, "fs_write");
    if (root == INVALID_HANDLE_VALUE) return -1;
    char pre[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 3 && prefix[i]; i++) pre[i] = prefix[i];
    unsigned seed = (unsigned)GetTickCount() ^ ((unsigned)GetCurrentProcessId() << 16) ^
                    ((unsigned)InterlockedIncrement(&g_win_temp_calls) * 0x9E3779B9u);
    for (int attempt = 0; attempt < 256; attempt++) {
        char name[32];
        seed = seed * 1103515245u + 12345u;
        snprintf(name, sizeof name, "%s%04X.tmp", pre, (seed >> 8) & 0xFFFF);
        wchar_t wname[64];
        if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64)) break;
        HANDLE h = INVALID_HANDLE_VALUE;
        NTSTATUS st = win_create_at(root, wname, is_dir ? FILE_LIST_DIRECTORY : GENERIC_WRITE,
                                    FILE_ATTRIBUTE_NORMAL, FILE_CREATE,
                                    (is_dir ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE) |
                                        FILE_OPEN_REPARSE_POINT,
                                    NULL, &h);
        if (st == STATUS_OBJECT_NAME_COLLISION || st == STATUS_DELETE_PENDING) continue;
        CloseHandle(root);
        if (st < 0) return win_fail_status(st);
        CloseHandle(h);
        size_t dl = strlen(dir);
        int sep = dl && dir[dl - 1] != '\\' && dir[dl - 1] != '/';
        if ((size_t)snprintf(out, cap, "%s%s%s", dir, sep ? "\\" : "", name) >= cap) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }
    CloseHandle(root);
    return win_fail(ERROR_FILE_EXISTS);
}

void* aether_sandbox_win_open(const char* path, unsigned long desired, unsigned long flags,
                              int nofollow, int access) {
    if (!path) { errno = EINVAL; return INVALID_HANDLE_VALUE; }
    char resolved[PATH_MAX];
    int levels = fs_levels_here();
    if (levels && (!(nofollow ? aether_sandbox_canon_entry(path, resolved, sizeof resolved)
                              : aether_sandbox_canon_path(path, resolved, sizeof resolved)) ||
                   !fs_access_allowed(resolved, access))) {
        fs_refused();
        return INVALID_HANDLE_VALUE;
    }
    wchar_t wp[AETHER_SANDBOX_WPATH];
    if (!win_wide(path, wp)) { win_fail(ERROR_INVALID_NAME); return INVALID_HANDLE_VALUE; }
    HANDLE h = CreateFileW(wp, (DWORD)desired | SYNCHRONIZE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           (DWORD)flags | FILE_FLAG_BACKUP_SEMANTICS |
                               (nofollow ? FILE_FLAG_OPEN_REPARSE_POINT : 0),
                           NULL);
    if (h == INVALID_HANDLE_VALUE) { win_fail(GetLastError()); return h; }
    if (levels && !win_handle_is(h, resolved, access)) {
        CloseHandle(h);
        fs_refused();
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

/* What `path` names, for a watch: a handle opened (overlapped, for
 * ReadDirectoryChangesW) and verified. */
void* aether_sandbox_win_open_watched(const char* path) {
    return aether_sandbox_win_open(path, FILE_LIST_DIRECTORY, FILE_FLAG_OVERLAPPED, 0, AETHER_ACCESS_READ);
}

/* A listing through a directory handle. NtQueryDirectoryFile with the
 * class FindFirstFile uses (FileBothDirectoryInformation), so a pattern
 * matches the long and the short name as it does there. */
typedef NTSTATUS (NTAPI* aether_nt_query_dir_fn)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID,
                                                 ULONG, int, BOOLEAN, PUNICODE_STRING, BOOLEAN);
typedef struct {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime, EndOfFile, AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    ULONG EaSize;
    CCHAR ShortNameLength;
    WCHAR ShortName[12];
    WCHAR FileName[1];
} AetherBothDirInfo;
#define AETHER_FILE_BOTH_DIR_INFO 3
#define AETHER_STATUS_NO_MORE_FILES ((NTSTATUS)0x80000006L)
#define AETHER_STATUS_NO_SUCH_FILE  ((NTSTATUS)0xC000000FL)

typedef struct {
    HANDLE h;
    int started;          /* the first query (with the pattern) was made */
    int done;
    ULONG at;             /* the next entry's offset in buf; `have` bytes filled */
    ULONG have;
    USHORT pat_len;       /* bytes; 0: every entry */
    WCHAR pat[MAX_PATH];
    ULONG_PTR buf[16384 / sizeof(ULONG_PTR)];   /* aligned for the entries */
} AetherWinDir;

/* FindFirstFile's wildcards in the file system's form, as it converts
 * them: '?' is DOS_QM ('>'); a '*' before a '.' is DOS_STAR ('<'); a '.'
 * before a '?' or '*' is DOS_DOT ('"'). So "*.*" matches a name with no
 * dot, as it does for FindFirstFile. */
static int win_dos_pattern(const char* pattern, AetherWinDir* d) {
    if (!pattern || !*pattern || strcmp(pattern, "*") == 0) { d->pat_len = 0; return 1; }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pattern, -1, d->pat, MAX_PATH);
    if (n <= 1) return 0;
    n--;
    for (int i = 0; i < n; i++) {
        WCHAR c = d->pat[i], next = i + 1 < n ? d->pat[i + 1] : 0;
        if (c == L'?') d->pat[i] = L'>';
        else if (c == L'*' && next == L'.') d->pat[i] = L'<';
        else if (c == L'.' && (next == L'?' || next == L'*')) d->pat[i] = L'"';
    }
    d->pat_len = (USHORT)(n * sizeof(WCHAR));
    return 1;
}

static AetherWinDir* win_dir_new(HANDLE h, const char* pattern) {
    AetherWinDir* d = (AetherWinDir*)calloc(1, sizeof *d);
    if (!d) { CloseHandle(h); errno = ENOMEM; return NULL; }
    d->h = h;
    if (!win_dos_pattern(pattern, d)) {
        CloseHandle(h);
        free(d);
        win_fail(ERROR_INVALID_NAME);
        return NULL;
    }
    return d;
}

void* aether_sandbox_win_opendir(const char* path, const char* pattern) {
    HANDLE h = (HANDLE)aether_sandbox_win_open(path, FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
                                              0, 0, AETHER_ACCESS_READ);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    return win_dir_new(h, pattern);
}

void* aether_sandbox_win_opendir_at(void* parent, const char* name) {
    AetherWinDir* p = (AetherWinDir*)parent;
    if (!p || !name) { errno = EINVAL; return NULL; }
    wchar_t wname[AETHER_SANDBOX_WPATH];
    if (!win_wide(name, wname)) { win_fail(ERROR_INVALID_NAME); return NULL; }
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS st = win_create_at(p->h, wname, FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES, 0,
                                FILE_OPEN, FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT, NULL, &h);
    if (st < 0) { win_fail_status(st); return NULL; }
    FILE_ATTRIBUTE_TAG_INFO ti;
    if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &ti, sizeof ti) ||
        ((ti.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(ti.ReparseTag))) {
        /* A link where the listing saw a directory: swapped in since. */
        CloseHandle(h);
        errno = ELOOP;
        return NULL;
    }
    return win_dir_new(h, NULL);
}

int aether_sandbox_win_readdir(void* dir, char* name, size_t cap, unsigned long* attrs) {
    AetherWinDir* d = (AetherWinDir*)dir;
    if (!d || !name || !cap) { errno = EINVAL; return -1; }
    aether_nt_query_dir_fn query = (aether_nt_query_dir_fn)win_ntdll("NtQueryDirectoryFile");
    if (!query) { win_fail(ERROR_NOT_SUPPORTED); return -1; }
    for (;;) {
        if (d->at >= d->have) {
            if (d->done) return 0;
            UNICODE_STRING us;
            us.Buffer = d->pat;
            us.Length = us.MaximumLength = d->pat_len;
            IO_STATUS_BLOCK io;
            NTSTATUS st = query(d->h, NULL, NULL, NULL, &io, d->buf, (ULONG)sizeof d->buf,
                                AETHER_FILE_BOTH_DIR_INFO, FALSE,
                                (!d->started && d->pat_len) ? &us : NULL, d->started ? FALSE : TRUE);
            d->started = 1;
            if (st == AETHER_STATUS_NO_MORE_FILES || st == AETHER_STATUS_NO_SUCH_FILE) {
                d->done = 1;
                return 0;
            }
            if (st < 0) { win_fail_status(st); return -1; }
            d->at = 0;
            d->have = (ULONG)io.Information;
            if (d->have == 0) { d->done = 1; return 0; }
        }
        const AetherBothDirInfo* e = (const AetherBothDirInfo*)((const unsigned char*)d->buf + d->at);
        d->at = e->NextEntryOffset ? d->at + e->NextEntryOffset : d->have;
        int wn = (int)(e->FileNameLength / sizeof(WCHAR));
        if ((wn == 1 && e->FileName[0] == L'.') ||
            (wn == 2 && e->FileName[0] == L'.' && e->FileName[1] == L'.'))
            continue;
        int n = WideCharToMultiByte(CP_UTF8, 0, e->FileName, wn, name, (int)cap - 1, NULL, NULL);
        if (n <= 0) continue;   /* longer than the caller holds: not listed */
        name[n] = '\0';
        if (attrs) *attrs = e->FileAttributes;
        return 1;
    }
}

void aether_sandbox_win_closedir(void* dir) {
    AetherWinDir* d = (AetherWinDir*)dir;
    if (!d) return;
    CloseHandle(d->h);
    free(d);
}

int aether_sandbox_win_handle_ok(void* handle, const char* path, int nofollow, int access) {
    if (!handle || !path || !fs_levels_here()) return 1;
    char resolved[PATH_MAX];
    if (!(nofollow ? aether_sandbox_canon_entry(path, resolved, sizeof resolved)
                   : aether_sandbox_canon_path(path, resolved, sizeof resolved)))
        return 0;
    return win_handle_is((HANDLE)handle, resolved, access);
}

/* fs.realpath inside an enforce block: the final path of what a handle
 * opened, once that is the file the check saw. */
int aether_sandbox_realpath(const char* path, char* out, size_t cap) {
    if (!path || !out) { errno = EINVAL; return -1; }
    int levels = fs_levels_here();
    char resolved[PATH_MAX];
    if (levels && (!aether_sandbox_canon_path(path, resolved, sizeof resolved) ||
                   decide("fs_read", resolved) == 0)) {
        fs_refused();
        return -1;
    }
    wchar_t wp[AETHER_SANDBOX_WPATH];
    if (!win_wide(path, wp)) return win_fail(ERROR_INVALID_NAME);
    HANDLE h = CreateFileW(wp, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return win_fail(GetLastError());
    int ok = !levels || win_handle_is(h, resolved, AETHER_ACCESS_READ);
    char fin[PATH_MAX];
    int named = ok && aether_sandbox_final_of_handle(h, fin, sizeof fin);
    CloseHandle(h);
    if (!ok) { fs_refused(); return -1; }
    if (!named) return win_fail(ERROR_INVALID_NAME);
    size_t n = strlen(fin);
    if (n + 1 > cap) { errno = ENAMETOOLONG; return -1; }
    memcpy(out, fin, n + 1);
    return 0;
}

#else

static const AetherSandboxFsOps g_libc_ops = {
    openat, close,
#if defined(__linux__)
    syscall,
#else
    NULL,
#endif
};

/* Under the LD_PRELOAD library a call is passed to its wrapper with the
 * resolved path: the wrapper checks its own policy and walks with the
 * functions behind it. Walking here would go through its wrappers one
 * directory at a time, each checked as an open of that directory. */
static int fs_preload_here(void) {
#ifdef RTLD_DEFAULT
    static _Atomic int known = 0;   /* 1 absent, 2 present */
    int k = atomic_load_explicit(&known, memory_order_relaxed);
    if (!k) {
        k = dlsym(RTLD_DEFAULT, "aether_sandbox_preload_present") ? 2 : 1;
        atomic_store_explicit(&known, k, memory_order_relaxed);
    }
    return k == 2;
#else
    return 0;
#endif
}

/* What an open with `flags` asks of the file (as the preload's open). */
static int fs_open_access(int flags) {
    int acc = flags & O_ACCMODE;
    int access = 0;
    if (acc == O_RDONLY || acc == O_RDWR) access |= AETHER_ACCESS_READ;
    if (acc != O_RDONLY || (flags & (O_CREAT | O_APPEND | O_TRUNC))) access |= AETHER_ACCESS_WRITE;
    return access;
}

int aether_sandbox_open(const char* path, int flags, int mode) {
    if (!path) { errno = EFAULT; return -1; }
    if (!fs_levels_here()) return open(path, flags, mode);
    /* O_NOFOLLOW and O_CREAT|O_EXCL act on a symlink at the end itself. */
    int entry = (flags & O_NOFOLLOW) || (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL);
    if (aether_sandbox_dir_slash(path)) {
        /* "name/": the kernel follows the name and needs a directory there,
         * and makes no file by such a path. */
        if (flags & O_CREAT) { errno = EISDIR; return -1; }
        entry = 0;
        flags |= O_DIRECTORY;
    }
    char resolved[PATH_MAX];
    int ok = entry ? aether_sandbox_canon_entry(path, resolved, sizeof resolved)
                   : aether_sandbox_canon_path(path, resolved, sizeof resolved);
    if (!ok || !fs_access_allowed(resolved, fs_open_access(flags))) {
        fs_refused();
        return -1;
    }
    if (fs_preload_here()) return open(resolved, flags, mode);
    return aether_sandbox_walk(resolved, flags, (unsigned)mode, &g_libc_ops);
}

void* aether_sandbox_fopen(const char* path, const char* mode) {
    if (!path || !mode || !fs_levels_here()) return fopen(path, mode);
    int flags = aether_fopen_flags(mode);
    if (flags < 0) { errno = EINVAL; return NULL; }
    int fd = aether_sandbox_open(path, flags, 0666);
    if (fd < 0) return NULL;
    FILE* fp = fdopen(fd, mode);
    if (!fp) {
        int err = errno;
        close(fd);
        errno = err;
    }
    return fp;
}

void* aether_sandbox_opendir(const char* path) {
    if (!path || !fs_levels_here()) return opendir(path);
    char resolved[PATH_MAX];
    if (!aether_sandbox_canon_path(path, resolved, sizeof resolved) ||
        decide("fs_read", resolved) == 0) {
        fs_refused();
        return NULL;
    }
    if (fs_preload_here()) return opendir(resolved);
    int fd = aether_sandbox_walk(resolved, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0, &g_libc_ops);
    if (fd < 0) return NULL;
    DIR* d = fdopendir(fd);
    if (!d) {
        int err = errno;
        close(fd);
        errno = err;
    }
    return d;
}

/* `resolved`'s directory opened as an anchor, `fn` called with it and the
 * last name, the anchor closed; fn's result and errno kept. */
typedef int (*fs_at_fn)(int dirfd, const char* name, const void* arg);
static int fs_at(const char* resolved, fs_at_fn fn, const void* arg) {
    char name[PATH_MAX];
    int d = aether_sandbox_anchor(resolved, name, sizeof name, &g_libc_ops);
    if (d < 0) return -1;
    int r = fn(d, name, arg);
    int err = errno;
    close(d);
    errno = err;
    return r;
}

static int at_unlink(int d, const char* n, const void* a) { (void)a; return unlinkat(d, n, 0); }
static int at_rmdir(int d, const char* n, const void* a) { (void)a; return unlinkat(d, n, AT_REMOVEDIR); }
static int at_remove(int d, const char* n, const void* a) {
    (void)a;
    struct stat st;
    if (fstatat(d, n, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode))
        return unlinkat(d, n, AT_REMOVEDIR);
    return unlinkat(d, n, 0);
}
static int at_mkdir(int d, const char* n, const void* a) { return mkdirat(d, n, (mode_t)*(const int*)a); }
static int at_symlink(int d, const char* n, const void* a) { return symlinkat((const char*)a, d, n); }
static int at_chmod(int d, const char* n, const void* a) {
#if defined(__wasi__)
    (void)d; (void)n; (void)a;
    errno = ENOTSUP;   /* WASI has no permission bits to change */
    return -1;
#else
    return aether_sandbox_chmod_at(d, n, (mode_t)*(const int*)a, &g_libc_ops, fchmodat, chmod);
#endif
}

/* A call on a name spelt "name/" (aether_sandbox_dir_slash) does what the
 * kernel does with it: 0 to go on, else -1 with its errno. */
static int fs_slash(const char* path, const char* resolved, int op) {
    if (!aether_sandbox_dir_slash(path)) return 0;
    return aether_sandbox_slash_call(AT_FDCWD, resolved, op);
}

int aether_sandbox_unlink(const char* path) {
    if (!path || !fs_levels_here()) return unlink(path);
    char resolved[PATH_MAX];
    if (!fs_entry_writable(path, resolved)) return -1;
    if (fs_slash(path, resolved, AETHER_SLASH_UNLINK) != 0) return -1;
    if (fs_preload_here()) return unlink(resolved);
    return fs_at(resolved, at_unlink, NULL);
}

int aether_sandbox_rmdir(const char* path) {
    if (!path || !fs_levels_here()) return rmdir(path);
    char resolved[PATH_MAX];
    if (!fs_entry_writable(path, resolved)) return -1;
    if (fs_slash(path, resolved, AETHER_SLASH_DIR) != 0) return -1;
    if (fs_preload_here()) return rmdir(resolved);
    return fs_at(resolved, at_rmdir, NULL);
}

int aether_sandbox_remove(const char* path) {
    if (!path || !fs_levels_here()) return remove(path);
    char resolved[PATH_MAX];
    if (!fs_entry_writable(path, resolved)) return -1;
    if (fs_slash(path, resolved, AETHER_SLASH_DIR) != 0) return -1;
    if (fs_preload_here()) return remove(resolved);
    return fs_at(resolved, at_remove, NULL);
}

int aether_sandbox_mkdir(const char* path, int mode) {
    if (!path || !fs_levels_here()) return mkdir(path, (mode_t)mode);
    char resolved[PATH_MAX];
    if (!fs_entry_writable(path, resolved)) return -1;
    if (fs_preload_here()) return mkdir(resolved, (mode_t)mode);
    return fs_at(resolved, at_mkdir, &mode);
}

/* mkdir of each prefix of `p` in turn, an existing directory taken as made. */
static int fs_mkdir_each(const char* p, int mode) {
    char buf[PATH_MAX];
    size_t len = strlen(p);
    if (len >= sizeof buf) { errno = ENAMETOOLONG; return -1; }
    memcpy(buf, p, len + 1);
    for (size_t i = 1; i <= len; i++) {
        if (buf[i] != '/' && buf[i] != '\0') continue;
        char saved = buf[i];
        buf[i] = '\0';
        if (mkdir(buf, (mode_t)mode) != 0) {
            int err = errno;
            struct stat st;
            if (err != EEXIST || stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) {
                errno = err == EEXIST ? ENOTDIR : err;
                return -1;
            }
        }
        buf[i] = saved;
    }
    return 0;
}

int aether_sandbox_mkdir_p(const char* path, int mode) {
    if (!path || !*path) { errno = EINVAL; return -1; }
    if (!fs_levels_here()) return fs_mkdir_each(path, mode);
    char resolved[PATH_MAX];
    if (!aether_sandbox_canon_path(path, resolved, sizeof resolved) ||
        decide("fs_write", resolved) == 0) {
        fs_refused();
        return -1;
    }
    if (fs_preload_here()) return fs_mkdir_each(resolved, mode);
    /* From "/" (on WASI the preopened directory that holds the path) down,
     * each directory opened as an anchor from the one before (O_NOFOLLOW),
     * and made first where it is missing. */
    const char* p = resolved;
    int owned = 1;
    int d = aether_sandbox_walk_start(resolved, &p, &owned, &g_libc_ops);
    if (d == -2) return fs_mkdir_each(resolved, mode);   /* no preopen holds it */
    if (d < 0) return -1;
    char name[PATH_MAX];
    for (;;) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* e = p;
        while (*e && *e != '/') e++;
        size_t n = (size_t)(e - p);
        memcpy(name, p, n);
        name[n] = '\0';
        int nd = openat(d, name, AETHER_SANDBOX_ANCHOR);
        if (nd < 0 && errno == ENOENT) {
            if (mkdirat(d, name, (mode_t)mode) != 0 && errno != EEXIST) {
                int err = errno;
                if (owned) close(d);
                errno = err;
                return -1;
            }
            nd = openat(d, name, AETHER_SANDBOX_ANCHOR);
        }
        if (nd < 0) {
            int err = errno;
            int link = aether_sandbox_met_link(d, name, err);
            if (owned) close(d);
            errno = link ? EACCES : err;
            return -1;
        }
        if (owned) close(d);
        d = nd;
        owned = 1;
        p = e;
    }
    if (owned) close(d);
    return 0;
}

int aether_sandbox_rename(const char* from, const char* to) {
    if (!from || !to || !fs_levels_here()) return rename(from, to);
    char rf[PATH_MAX], rt[PATH_MAX];
    if (!fs_entry_writable(from, rf) || !fs_entry_writable(to, rt)) return -1;
    if (aether_sandbox_dir_slash(from) || aether_sandbox_dir_slash(to)) {
        /* A directory only, onto a directory or nothing, as the kernel. */
        if (aether_sandbox_slash_call(AT_FDCWD, rf, AETHER_SLASH_DIR) != 0) return -1;
        struct stat st;
        if (lstat(rt, &st) == 0 && !S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    }
    if (fs_preload_here()) return rename(rf, rt);
    char nf[PATH_MAX], nt[PATH_MAX];
    int df = aether_sandbox_anchor(rf, nf, sizeof nf, &g_libc_ops);
    if (df < 0) return -1;
    int dt = aether_sandbox_anchor(rt, nt, sizeof nt, &g_libc_ops);
    if (dt < 0) {
        int err = errno;
        close(df);
        errno = err;
        return -1;
    }
    int r = renameat(df, nf, dt, nt);
    int err = errno;
    close(df);
    close(dt);
    errno = err;
    return r;
}

int aether_sandbox_symlink(const char* target, const char* linkpath) {
    if (!target || !linkpath || !fs_levels_here()) return symlink(target, linkpath);
    /* The target is text the link holds; whatever opens the link later is
     * checked where it leads. */
    char resolved[PATH_MAX];
    if (!fs_entry_writable(linkpath, resolved)) return -1;
    if (fs_slash(linkpath, resolved, AETHER_SLASH_NEW) != 0) return -1;
    if (fs_preload_here()) return symlink(target, resolved);
    return fs_at(resolved, at_symlink, target);
}

int aether_sandbox_link(const char* target, const char* linkpath) {
    if (!target || !linkpath || !fs_levels_here()) return link(target, linkpath);
    char rl[PATH_MAX], rt[PATH_MAX];
    if (!fs_link_allowed(target, linkpath, rt, rl)) return -1;
    if (fs_slash(linkpath, rl, AETHER_SLASH_NEW) != 0) return -1;
    if (aether_sandbox_dir_slash(target)) {
        /* "name/" as the source: followed and refused, as the kernel does. */
        struct stat st;
        if (stat(rt, &st) != 0) errno = ENOENT;
        else errno = S_ISDIR(st.st_mode) ? EPERM : ENOTDIR;
        return -1;
    }
    if (fs_preload_here()) return link(rt, rl);
    char nt[PATH_MAX], nl[PATH_MAX];
    int dt = aether_sandbox_anchor(rt, nt, sizeof nt, &g_libc_ops);
    if (dt < 0) return -1;
    int dl = aether_sandbox_anchor(rl, nl, sizeof nl, &g_libc_ops);
    if (dl < 0) {
        int err = errno;
        close(dt);
        errno = err;
        return -1;
    }
    int r = linkat(dt, nt, dl, nl, 0);
    int err = errno;
    close(dt);
    close(dl);
    errno = err;
    return r;
}

static int at_utimens(int d, const char* n, const void* a) {
    /* The resolution has no symlink at its end: one there now is the race,
     * and its own times are what is set, not what it points at. */
    return utimensat(d, n, (const struct timespec*)a, AT_SYMLINK_NOFOLLOW);
}

int aether_sandbox_utimens(const char* path, const void* ts, int nofollow) {
    if (!path || !fs_levels_here())
        return utimensat(AT_FDCWD, path, (const struct timespec*)ts, nofollow ? AT_SYMLINK_NOFOLLOW : 0);
    char resolved[PATH_MAX];
    if (!(nofollow ? aether_sandbox_canon_entry(path, resolved, sizeof resolved)
                   : aether_sandbox_canon_path(path, resolved, sizeof resolved)) ||
        decide("fs_write", resolved) == 0) {
        fs_refused();
        return -1;
    }
    if (fs_preload_here())
        return utimensat(AT_FDCWD, resolved, (const struct timespec*)ts, AT_SYMLINK_NOFOLLOW);
    return fs_at(resolved, at_utimens, ts);
}

int aether_sandbox_chmod(const char* path, int mode) {
#if defined(__wasi__)
    if (!path || !fs_levels_here()) { errno = ENOTSUP; return -1; }
#else
    if (!path || !fs_levels_here()) return chmod(path, (mode_t)mode);
#endif
    /* chmod follows a symlink: the file it reaches is checked. */
    char resolved[PATH_MAX];
    if (!aether_sandbox_canon_path(path, resolved, sizeof resolved) ||
        decide("fs_write", resolved) == 0) {
        fs_refused();
        return -1;
    }
#if !defined(__wasi__)
    if (fs_preload_here()) return chmod(resolved, (mode_t)mode);
#endif
    return fs_at(resolved, at_chmod, &mode);
}


/* The functions lookups walk with. Under the LD_PRELOAD library its own
 * real ones (aether_sandbox_preload_ops): a lookup is not something its
 * policy holds, and through its openat each anchor would be checked as an
 * open of that directory. NULL when the preload gives none. */
static const AetherSandboxFsOps* fs_lookup_ops(void) {
    if (!fs_preload_here()) return &g_libc_ops;
#ifdef RTLD_DEFAULT
    static const AetherSandboxFsOps* _Atomic pre = NULL;
    const AetherSandboxFsOps* p = atomic_load_explicit(&pre, memory_order_acquire);
    if (!p) {
        void* sym = dlsym(RTLD_DEFAULT, "aether_sandbox_preload_ops");
        const AetherSandboxFsOps* (*get)(void);
        memcpy(&get, &sym, sizeof get);
        p = sym ? get() : NULL;
        if (p) atomic_store_explicit(&pre, p, memory_order_release);
    }
    return p;
#else
    return NULL;
#endif
}

static void fs_fill_stat(const struct stat* st, AetherSandboxStat* out) {
    memset(out, 0, sizeof *out);
    out->mode = (unsigned)st->st_mode;
    out->size = (long long)st->st_size;
    out->mtime = (long long)st->st_mtime;
    out->kind = S_ISLNK(st->st_mode) ? AETHER_SANDBOX_KIND_SYMLINK
              : S_ISDIR(st->st_mode) ? AETHER_SANDBOX_KIND_DIR
              : S_ISREG(st->st_mode) ? AETHER_SANDBOX_KIND_FILE
                                     : AETHER_SANDBOX_KIND_OTHER;
}

/* The lookups: fs_read on the resolution (the name itself with
 * `nofollow`), then fstatat of the name from its anchor, not following it:
 * a resolution has no symlink at its end, so one there now is the race. */
static int fs_lookup_at(const char* path, int nofollow, char resolved[PATH_MAX],
                        int (*fn)(int dirfd, const char* name, void* arg), void* arg) {
    if (!(nofollow ? aether_sandbox_canon_entry(path, resolved, PATH_MAX)
                   : aether_sandbox_canon_path(path, resolved, PATH_MAX)) ||
        decide("fs_read", resolved) == 0) {
        fs_refused();
        return -1;
    }
    const AetherSandboxFsOps* ops = fs_lookup_ops();
    if (!ops || strcmp(resolved, "/") == 0) return fn(AT_FDCWD, resolved, arg);
    char name[PATH_MAX];
    int d = aether_sandbox_anchor(resolved, name, sizeof name, ops);
    if (d < 0) return -1;
    int r = fn(d, name, arg);
    int err = errno;
    ops->close(d);
    errno = err;
    return r;
}

typedef struct { struct stat st; int nofollow; } FsStatArg;
static int at_stat(int d, const char* n, void* a) {
    FsStatArg* s = (FsStatArg*)a;
    if (fstatat(d, n, &s->st, AT_SYMLINK_NOFOLLOW) != 0) return -1;
    if (!s->nofollow && S_ISLNK(s->st.st_mode) && d != AT_FDCWD) { fs_refused(); return -1; }
    return 0;
}

int aether_sandbox_stat(const char* path, int nofollow, AetherSandboxStat* out) {
    if (!path || !out) { errno = EINVAL; return -1; }
    FsStatArg a;
    a.nofollow = nofollow;
    if (!fs_levels_here()) {
        if ((nofollow ? lstat(path, &a.st) : stat(path, &a.st)) != 0) return -1;
        fs_fill_stat(&a.st, out);
        return 0;
    }
    char resolved[PATH_MAX];
    if (fs_lookup_at(path, nofollow, resolved, at_stat, &a) != 0) return -1;
    fs_fill_stat(&a.st, out);
    return 0;
}

typedef struct { char* buf; size_t cap; ssize_t n; } FsLinkArg;
static int at_readlink(int d, const char* n, void* a) {
    FsLinkArg* l = (FsLinkArg*)a;
    l->n = readlinkat(d, n, l->buf, l->cap);
    return l->n < 0 ? -1 : 0;
}

long aether_sandbox_readlink(const char* path, char* buf, size_t cap) {
    if (!path || !buf) { errno = EINVAL; return -1; }
    if (!fs_levels_here()) return (long)readlink(path, buf, cap);
    FsLinkArg l = { buf, cap, -1 };
    char resolved[PATH_MAX];
    if (fs_lookup_at(path, 1, resolved, at_readlink, &l) != 0) return -1;
    return (long)l.n;
}

/* fs.realpath inside an enforce block: the resolution the check matched,
 * once a lookup through its anchor finds something there. */
int aether_sandbox_realpath(const char* path, char* out, size_t cap) {
    if (!path || !out) { errno = EINVAL; return -1; }
    if (!fs_levels_here()) {
        char* r = realpath(path, NULL);
        if (!r) return -1;
        size_t n = strlen(r);
        if (n + 1 > cap) { free(r); errno = ENAMETOOLONG; return -1; }
        memcpy(out, r, n + 1);
        free(r);
        return 0;
    }
    FsStatArg a;
    a.nofollow = 0;
    char resolved[PATH_MAX];
    if (fs_lookup_at(path, 0, resolved, at_stat, &a) != 0) return -1;
    size_t n = strlen(resolved);
    if (n + 1 > cap) { errno = ENAMETOOLONG; return -1; }
    memcpy(out, resolved, n + 1);
    return 0;
}

/* A descriptor of what `path` names, for asking about it (fstatvfs, a
 * watch through /proc/self/fd): fs_read on it, opened by the walk as a
 * path only (O_PATH, or O_RDONLY where the system has no O_PATH). */
int aether_sandbox_open_lookup(const char* path) {
#ifdef O_PATH
    const int flags = O_PATH | O_CLOEXEC;
#else
    const int flags = O_RDONLY | O_CLOEXEC;
#endif
    if (!path) { errno = EINVAL; return -1; }
    if (!fs_levels_here()) return open(path, flags);
    char resolved[PATH_MAX];
    if (!aether_sandbox_canon_path(path, resolved, sizeof resolved) || decide("fs_read", resolved) == 0) {
        fs_refused();
        return -1;
    }
    const AetherSandboxFsOps* ops = fs_lookup_ops();
#ifndef O_PATH
    /* A readable descriptor is more than a lookup: under the preload it is
     * opened through its open, which checks its grants too. */
    if (fs_preload_here()) ops = NULL;
#endif
    if (!ops) return open(resolved, flags);
    return aether_sandbox_walk(resolved, flags, 0, ops);
}

/* fs.write_atomic: the temporary beside `path` is made by openat from the
 * anchor of `path`'s directory, and renamed onto `path` from it, so the
 * file written, the temporary and the name replaced are in the directory
 * the check saw. -2 when this is not the way here (no level in force, or
 * the preload, whose staged open and rename do the same): the caller makes
 * its own. */
static atomic_ulong g_stage_counter;

int aether_sandbox_stage_begin(const char* path, AetherSandboxStaged* s) {
    if (!path || !s) { errno = EINVAL; return -1; }
    memset(s, 0, sizeof *s);
    s->anchor = -1;
    if (!fs_levels_here() || fs_preload_here()) return -2;
    char resolved[PATH_MAX];
    if (!aether_sandbox_canon_entry(path, resolved, sizeof resolved) || decide("fs_write", resolved) == 0) {
        fs_refused();
        return -1;
    }
    int d = aether_sandbox_anchor(resolved, s->name, sizeof s->name, &g_libc_ops);
    if (d < 0) return -1;
    for (int attempt = 0; attempt < 16; attempt++) {
        unsigned long n = atomic_fetch_add(&g_stage_counter, 1) + 1;
        if ((size_t)snprintf(s->tmp, sizeof s->tmp, "%s.tmp.%ld.%lu", s->name, (long)getpid(), n) >=
            sizeof s->tmp) {
            close(d);
            errno = ENAMETOOLONG;
            return -1;
        }
        int fd = openat(d, s->tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
        if (fd >= 0) {
            s->anchor = d;
            return fd;
        }
        if (errno != EEXIST) break;
    }
    int err = errno;
    close(d);
    errno = err;
    return -1;
}

int aether_sandbox_stage_commit(AetherSandboxStaged* s) {
    int d = (int)s->anchor;
    int r = renameat(d, s->tmp, d, s->name);
    int err = errno;
    if (r != 0) unlinkat(d, s->tmp, 0);
    close(d);
    errno = err;
    return r;
}

void aether_sandbox_stage_abort(AetherSandboxStaged* s) {
    int d = (int)s->anchor;
    int err = errno;
    unlinkat(d, s->tmp, 0);
    close(d);
    errno = err;
}

/* fs.make_temp_file and fs.make_temp_dir: `dir` checked for fs_write (as
 * std always checked it) and opened as an anchor by the walk, and the new
 * name made from it with O_CREAT|O_EXCL or mkdirat, so it is made in the
 * directory that was checked. The path returned is `dir` as given and the
 * name, as mkstemp's template made it. -2 when this is not the way here:
 * the caller makes its own (the preload's mkstemp and mkdtemp do the same). */
int aether_sandbox_temp_in(const char* dir, const char* prefix, int is_dir, char* out, size_t cap) {
    if (!dir || !prefix || !out) { errno = EINVAL; return -1; }
    if (!fs_levels_here() || fs_preload_here()) return -2;
    char rdir[PATH_MAX];
    if (!aether_sandbox_canon_path(dir, rdir, sizeof rdir) || decide("fs_write", rdir) == 0) {
        fs_refused();
        return -1;
    }
    int d = aether_sandbox_walk(rdir, AETHER_SANDBOX_ANCHOR & ~O_NOFOLLOW, 0, &g_libc_ops);
    if (d < 0) return -1;
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    unsigned long seed = (unsigned long)getpid() * 2654435761u ^ (unsigned long)time(NULL) ^
                         (unsigned long)(size_t)out;
    for (int attempt = 0; attempt < 256; attempt++) {
        char name[NAME_MAX + 1];
        size_t pl = strlen(prefix);
        if (pl + 7 > sizeof name) { close(d); errno = ENAMETOOLONG; return -1; }
        memcpy(name, prefix, pl);
        seed ^= atomic_fetch_add(&g_stage_counter, 1) * 0x9E3779B97F4A7C15ull;
        unsigned long v = seed;
        for (int i = 0; i < 6; i++) {
            name[pl + (size_t)i] = chars[v % (sizeof chars - 1)];
            v = v / (sizeof chars - 1) + (v << 7);
        }
        name[pl + 6] = '\0';
        int r = is_dir ? mkdirat(d, name, 0700)
                       : openat(d, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (r >= 0) {
            if (!is_dir) close(r);
            close(d);
            size_t dl = strlen(dir);
            int sep = dl && dir[dl - 1] != '/';
            if ((size_t)snprintf(out, cap, "%s%s%s", dir, sep ? "/" : "", name) >= cap) {
                errno = ENAMETOOLONG;
                return -1;
            }
            return 0;
        }
        if (errno != EEXIST) break;
    }
    int err = errno;
    close(d);
    errno = err;
    return -1;
}
#endif /* _WIN32 */

// ---------------------------------------------------------------------------
// A staged write (fs.write_atomic)
// ---------------------------------------------------------------------------

/* The temporary fs.write_atomic is about to create, and the file it stands
 * for, whose fs_write the caller has checked. Thread-local, set around that
 * one open: the LD_PRELOAD library asks aether_sandbox_staged_for and checks
 * the file instead of the temporary's own name (libaether_sandbox_preload.c,
 * staged_write_target). Borrowed: the caller keeps both alive while set. */
#if defined(AETHER_NO_THREADING)
typedef struct { const char* tmp; const char* path; } SandboxStage;
static const char t_stage_key = 0;
#define t_stage_tmp  (((SandboxStage*)sandbox_task_block(&t_stage_key, sizeof(SandboxStage)))->tmp)
#define t_stage_path (((SandboxStage*)sandbox_task_block(&t_stage_key, sizeof(SandboxStage)))->path)
#else
static AETHER_TLS_SHARED const char* t_stage_tmp;
static AETHER_TLS_SHARED const char* t_stage_path;
#endif

void aether_sandbox_stage(const char* tmp, const char* path) {
    t_stage_tmp = tmp && path ? tmp : NULL;
    t_stage_path = tmp && path ? path : NULL;
}

const char* aether_sandbox_staged_for(const char* tmp) {
    if (!tmp || !t_stage_tmp || strcmp(tmp, t_stage_tmp) != 0) return NULL;
    return t_stage_path;
}
#elif defined(_WIN32)
// No sandbox compiled in: std still opens through this one (tar's times,
// chmod's handle), and with no level in force it is the plain open.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>

void* aether_sandbox_win_open(const char* path, unsigned long desired, unsigned long flags,
                              int nofollow, int access) {
    (void)access;
    if (!path) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return INVALID_HANDLE_VALUE;
    wchar_t* w = (wchar_t*)malloc((size_t)wlen * sizeof(wchar_t));
    if (!w) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE_VALUE; }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, wlen);
    DWORD f = (DWORD)flags | FILE_FLAG_BACKUP_SEMANTICS;
    if (nofollow) f |= FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE h = CreateFileW(w, (DWORD)desired, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, f, NULL);
    DWORD e = GetLastError();
    free(w);
    SetLastError(e);
    return h;
}
#endif
