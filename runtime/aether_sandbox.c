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
#elif defined(__unix__) || defined(__APPLE__)
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
static AETHER_TLS_SHARED int t_total;

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

static AETHER_TLS_SHARED SandboxThread t_sbx;

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

/* 1 allowed, 0 refused, -1 nothing in force on this thread. */
static int decide(const char* category, const char* resource) {
    SandboxThread* t = &t_sbx;
    int total = rv()->levels(0);
    if (total == 0 && t->depth == t->floor) return -1;
    if (atomic_load_explicit(&g_hooks_unserved, memory_order_acquire)) {
        aether_sandbox_audit(category, resource, 0);   /* unserved: refused more, never less */
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
        aether_sandbox_audit(category, resource, 0);
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
    aether_sandbox_audit(category, resource, allowed);
    return allowed;
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
// A staged write (fs.write_atomic)
// ---------------------------------------------------------------------------

/* The temporary fs.write_atomic is about to create, and the file it stands
 * for, whose fs_write the caller has checked. Thread-local, set around that
 * one open: the LD_PRELOAD library asks aether_sandbox_staged_for and checks
 * the file instead of the temporary's own name (libaether_sandbox_preload.c,
 * staged_write_target). Borrowed: the caller keeps both alive while set. */
static AETHER_TLS_SHARED const char* t_stage_tmp;
static AETHER_TLS_SHARED const char* t_stage_path;

void aether_sandbox_stage(const char* tmp, const char* path) {
    t_stage_tmp = tmp && path ? tmp : NULL;
    t_stage_path = tmp && path ? path : NULL;
}

const char* aether_sandbox_staged_for(const char* tmp) {
    if (!tmp || !t_stage_tmp || strcmp(tmp, t_stage_tmp) != 0) return NULL;
    return t_stage_path;
}
#endif
