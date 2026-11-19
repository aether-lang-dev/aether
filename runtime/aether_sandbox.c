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
#  include <io.h>            /* _get_osfhandle: aether_sandbox_fopen */
#else
#  include <dirent.h>        /* fdopendir: aether_sandbox_opendir */
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
static AETHER_TLS_SHARED int t_fs_refused;

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

#ifdef _WIN32

/* Windows: no descriptor-relative calls to make the open from, so the file
 * is opened as before and what was opened is checked again: the final path
 * of its handle, every junction and symlink followed, must be granted too.
 * A junction swapped in between the check and the open is caught there,
 * after the open: a "w" open has then truncated the file it reached. */
void* aether_sandbox_fopen(const char* path, const char* mode) {
    if (!path || !mode || !fs_levels_here()) return fopen(path, mode);
    char resolved[PATH_MAX];
    int access = aether_fopen_access(mode);
    if (!aether_sandbox_canon_path(path, resolved, sizeof resolved) ||
        !fs_access_allowed(resolved, access)) {
        fs_refused();
        return NULL;
    }
    FILE* fp = fopen(path, mode);
    if (!fp) return NULL;
    char fin[PATH_MAX];
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(fp));
    if (h == INVALID_HANDLE_VALUE || !aether_sandbox_final_of_handle(h, fin, sizeof fin) ||
        (strcmp(fin, resolved) != 0 && !fs_access_allowed(fin, access))) {
        fclose(fp);
        fs_refused();
        return NULL;
    }
    return fp;
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
    mode_t mode = (mode_t)*(const int*)a;
    if (fchmodat(d, n, mode, AT_SYMLINK_NOFOLLOW) == 0) return 0;
    if (errno != ENOTSUP && errno != EOPNOTSUPP && errno != EINVAL) return -1;
    /* No chmod of a name without following it here (glibc before 2.32, or
     * a symlink): the name was resolved, so a symlink there now is the
     * race. Otherwise chmod it by name; only the last name is then open to
     * a swap, between this look and the call. */
    struct stat st;
    if (fstatat(d, n, &st, AT_SYMLINK_NOFOLLOW) != 0) return -1;
    if (S_ISLNK(st.st_mode)) { fs_refused(); return -1; }
    return fchmodat(d, n, mode, 0);
#endif
}

int aether_sandbox_unlink(const char* path) {
    if (!path || !fs_levels_here()) return unlink(path);
    char resolved[PATH_MAX];
    if (!fs_entry_writable(path, resolved)) return -1;
    if (fs_preload_here()) return unlink(resolved);
    return fs_at(resolved, at_unlink, NULL);
}

int aether_sandbox_rmdir(const char* path) {
    if (!path || !fs_levels_here()) return rmdir(path);
    char resolved[PATH_MAX];
    if (!fs_entry_writable(path, resolved)) return -1;
    if (fs_preload_here()) return rmdir(resolved);
    return fs_at(resolved, at_rmdir, NULL);
}

int aether_sandbox_remove(const char* path) {
    if (!path || !fs_levels_here()) return remove(path);
    char resolved[PATH_MAX];
    if (!fs_entry_writable(path, resolved)) return -1;
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
    /* From "/" down, each directory opened as an anchor from the one before
     * (O_NOFOLLOW), and made first where it is missing. */
    int d = open("/", AETHER_SANDBOX_ANCHOR);
    if (d < 0) return -1;
    const char* p = resolved;
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
                close(d);
                errno = err;
                return -1;
            }
            nd = openat(d, name, AETHER_SANDBOX_ANCHOR);
        }
        if (nd < 0) {
            int err = errno;
            int link = aether_sandbox_met_link(d, name, err);
            close(d);
            errno = link ? EACCES : err;
            return -1;
        }
        close(d);
        d = nd;
        p = e;
    }
    close(d);
    return 0;
}

int aether_sandbox_rename(const char* from, const char* to) {
    if (!from || !to || !fs_levels_here()) return rename(from, to);
    char rf[PATH_MAX], rt[PATH_MAX];
    if (!fs_entry_writable(from, rf) || !fs_entry_writable(to, rt)) return -1;
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
    if (fs_preload_here()) return symlink(target, resolved);
    return fs_at(resolved, at_symlink, target);
}

int aether_sandbox_link(const char* target, const char* linkpath) {
    if (!target || !linkpath || !fs_levels_here()) return link(target, linkpath);
    char rl[PATH_MAX], rt[PATH_MAX];
    if (!fs_link_allowed(target, linkpath, rt, rl)) return -1;
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

#endif /* _WIN32 */

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
