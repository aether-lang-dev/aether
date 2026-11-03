/* aether_resource_caps.c — runtime per-call resource caps (#343).
 *
 * See aether_resource_caps.h for the API contract. This file owns
 * the atomics, the TLS slots, and the clock arithmetic; everything
 * else (cap-aware allocators, codegen tripwire, public libaether
 * forwarders) calls in via the header.
 */

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE   /* RTLD_DEFAULT in glibc's <dlfcn.h> */
#endif
#include "aether_resource_caps.h"
#include "utils/aether_thread.h"
#include "utils/aether_compiler.h"
#include "actors/aether_frames.h"
#include "aether_rv_mark.h"

/* The host API (libaether.h): exported however the runtime is built. */
#if defined(__GNUC__) || defined(__clang__)
#  define AETHER_CAPS_API __attribute__((visibility("default")))
#else
#  define AETHER_CAPS_API
#endif

#include <stdatomic.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>
#include <stddef.h>   /* offsetof: the marks (#3445) */

#ifdef _WIN32
#  include <windows.h>
#  include <tlhelp32.h>   /* the module list, under the loader's lock (#3445) */
#endif
#if !defined(_WIN32) && !defined(AETHER_NO_THREADING)
#  include <sched.h>
#endif
#if !defined(_WIN32) && (defined(__unix__) || defined(__APPLE__))
#  include <dlfcn.h>
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>
#    include <mach-o/getsect.h>
#  elif defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#    include <link.h>
#    define AETHER_RV_PHDR 1
#  endif
#endif

/* Process-wide. _Atomic for the lock-free fast path — every alloc
 * site touches g_mem_used; contention is real but bounded by the
 * compare-and-add pattern. */
static _Atomic uint64_t g_mem_used = 0;
static _Atomic uint64_t g_mem_cap  = 0;   /* 0 = unlimited */

/* What the allocation fast path must ask about, in one word it reads once
 * (#2954): a cap is set, an execution account exists, the test hook is
 * armed. None of them (AETHER_CAPS_ALLOC_BITS clear), and an allocation only
 * counts its bytes. The word's other bits are the step barrier's
 * (aether_resource_caps.h). */
#define CAPS_SLOW_CAP      1u
#define CAPS_SLOW_ACCOUNTS 2u
#define CAPS_SLOW_TEST     4u
_Static_assert(AETHER_CAPS_ALLOC_BITS == (CAPS_SLOW_CAP | CAPS_SLOW_ACCOUNTS | CAPS_SLOW_TEST),
               "the allocation fast path tests exactly its own bits");
_Static_assert(CAPS_SLOW_ACCOUNTS == AETHER_CAPS_SLOW_ACCOUNTS, "one accounts bit");
_Static_assert(((CAPS_SLOW_CAP | CAPS_SLOW_TEST) & AETHER_CAPS_STEP_BITS) == 0 &&
               (AETHER_CAPS_SLOW_SANDBOX & AETHER_CAPS_ALLOC_BITS) == 0 &&
               (AETHER_CAPS_SPAWN_MASK & (AETHER_CAPS_ALLOC_BITS | AETHER_CAPS_SLOW_SANDBOX)) == 0,
               "the step's bits and the allocation's share only the accounts bit");
_Atomic unsigned aether_caps_slow = 0;

/* Per-thread. The deadline is monotonic-clock nanoseconds; 0 means
 * "no deadline armed". The tripped flag is sticky once set, until
 * aether_caps_set_deadline_ms is called again. */
static AETHER_TLS_SHARED int64_t g_deadline_at_ns = 0;
static AETHER_TLS_SHARED int     g_tripped        = 0;

/* Monotonic nanoseconds since an arbitrary epoch; only differences
 * matter. Delegates to aether_now_ns so Windows goes through the
 * overflow-free QueryPerformanceCounter split in the thread shim. */
static int64_t now_ns(void) {
    return (int64_t)aether_now_ns();
}

/* `ms` (> 0) milliseconds in nanoseconds, and `ms` from now as a time on
 * now_ns's clock, each held at INT64_MAX past the end of what an int64_t
 * counts: a time no clock reaches, so a deadline that far off never trips
 * (#3528). `ms * 1000000LL` overflowed for anything past about 292 years,
 * a host passing long.max to mean "no deadline" among them, and the
 * runtime is not built with -fwrapv: in practice the sum landed in the
 * past and the work stopped at once. */
static int64_t ms_to_ns(int64_t ms) {
    if (ms > INT64_MAX / 1000000LL) return INT64_MAX;
    return ms * 1000000LL;
}

static int64_t ns_after_ms(int64_t ms) {
    int64_t now = now_ns();
    int64_t span = ms_to_ns(ms);
    if (now > 0 && span > INT64_MAX - now) return INT64_MAX;
    return now + span;
}

int aether_caps_check_alloc(size_t bytes) {
    /* Fast path: no cap set. One relaxed load + branch. */
    uint64_t cap = atomic_load_explicit(&g_mem_cap, memory_order_relaxed);
    if (cap == 0) {
        /* Still account the alloc so the counter reflects current
         * usage even when no cap is active — keeps test harnesses
         * and future cap-set transitions consistent. */
        atomic_fetch_add_explicit(&g_mem_used, (uint64_t)bytes,
                                  memory_order_relaxed);
        return 1;
    }

    /* Cap-set path: CAS-add to keep current+bytes <= cap. Loop
     * because another thread may bump the counter between our load
     * and our CAS — re-read and retry. */
    uint64_t cur = atomic_load_explicit(&g_mem_used, memory_order_relaxed);
    for (;;) {
        if (cur + (uint64_t)bytes > cap || cur + (uint64_t)bytes < cur) {
            return 0;  /* would exceed cap or wraps */
        }
        if (atomic_compare_exchange_weak_explicit(
                &g_mem_used, &cur, cur + (uint64_t)bytes,
                memory_order_relaxed, memory_order_relaxed)) {
            return 1;
        }
        /* CAS failed; cur is now the latest observed value. Retry. */
    }
}

void aether_caps_account_free(size_t bytes) {
    /* Saturating decrement: never wrap below 0. Underflow would
     * falsely trip the cap on subsequent allocations. */
    uint64_t cur = atomic_load_explicit(&g_mem_used, memory_order_relaxed);
    for (;;) {
        uint64_t next = (cur >= (uint64_t)bytes) ? (cur - (uint64_t)bytes) : 0;
#ifndef NDEBUG
        assert(cur >= (uint64_t)bytes && "alloc/free accounting underflow");
#endif
        if (atomic_compare_exchange_weak_explicit(
                &g_mem_used, &cur, next,
                memory_order_relaxed, memory_order_relaxed)) {
            return;
        }
    }
}

void aether_caps_set_memory_cap(uint64_t bytes) {
    atomic_store_explicit(&g_mem_cap, bytes, memory_order_relaxed);
    if (bytes) atomic_fetch_or_explicit(&aether_caps_slow, CAPS_SLOW_CAP, memory_order_relaxed);
    else atomic_fetch_and_explicit(&aether_caps_slow, ~CAPS_SLOW_CAP, memory_order_relaxed);
}

/* Nonzero once any thread has armed or tripped a deadline, or cancelled a
 * call (aether_exec.c). The codegen's loop-head check reads this first, so
 * code that never uses deadlines pays one load instead of a call and two
 * TLS reads. It is never cleared: another thread may still be armed, and
 * the full check below stays exact.
 *
 * Atomic, read with relaxed loads (a plain load on x86 and ARM): a cancel
 * comes from another thread while the call runs, and a loop that started
 * before it must see it. As a plain int, clang read it once before a loop
 * whose body calls nothing and never again, so the loop ran to its end. */
_Atomic int aether_caps_armed = 0;

static inline void caps_arm(void) {
    atomic_store_explicit(&aether_caps_armed, 1, memory_order_relaxed);
}

void aether_caps_set_deadline_ms(int64_t ms) {
    if (ms > 0) caps_arm();
    if (ms <= 0) {
        g_deadline_at_ns = 0;
    } else {
        g_deadline_at_ns = ns_after_ms(ms);
    }
    /* Reset the sticky flag — a fresh deadline is a fresh chance. */
    g_tripped = 0;
}

int aether_caps_deadline_tripped(void) {
    if (g_tripped) return 1;
    if (g_deadline_at_ns == 0) return 0;
    if (now_ns() >= g_deadline_at_ns) {
        caps_arm();
        g_tripped = 1;
        return 1;
    }
    return 0;
}

void __aether_abort_call(void) {
    caps_arm();
    g_tripped = 1;
}

void aether_caps_deadline_save(AetherDeadlineState* out) {
    out->at_ns = g_deadline_at_ns;
    out->tripped = g_tripped;
}

void aether_caps_deadline_restore(const AetherDeadlineState* s) {
    if (s->at_ns || s->tripped) caps_arm();
    g_deadline_at_ns = s->at_ns;
    g_tripped = s->tripped;
}

AetherDeadlineState aether_caps_deadline_for_ms(int64_t ms) {
    AetherDeadlineState s;
    s.at_ns = ms > 0 ? ns_after_ms(ms) : 0;
    s.tripped = 0;
    return s;
}

int aether_caps_deadline_flag(void) {
    return g_tripped;
}

uint64_t aether_caps_used_bytes(void) {
    return atomic_load_explicit(&g_mem_used, memory_order_relaxed);
}

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------
 * Allocation failure inside an execution (#2956)
 * ------------------------------------------------------------------
 * A cap-aware allocation that fails, past the cap or with the allocator
 * out of memory, returns NULL to its caller, which fails cleanly (an
 * error, its own memory freed), as it does outside an execution. Inside
 * one the failure is also noted on the innermost execution, and its next
 * safe point, or its return, ends it with AETHER_MEMORY_CAP or
 * AETHER_OUT_OF_MEMORY. The call does not unwind from inside the
 * allocator: that would skip the C code between it and the safe point,
 * which holds memory and may hold a lock. */

/* aether_test_fail_alloc_after: one global load per allocation while no
 * test has asked, the count per thread so a test is deterministic. The flag
 * is read by generated code too (_aether_alloc, #3377), so it is exported. */
_Atomic int aether_alloc_test_armed = 0;
static AETHER_TLS_SHARED int t_alloc_fail_after = 0;

void aether_test_fail_alloc_after(int n) {
    t_alloc_fail_after = n > 0 ? n : 0;
    if (n > 0) {
        atomic_store_explicit(&aether_alloc_test_armed, 1, memory_order_relaxed);
        atomic_fetch_or_explicit(&aether_caps_slow, CAPS_SLOW_TEST, memory_order_relaxed);
    }
}

int aether_alloc_test_refuses(void) {
    if (!atomic_load_explicit(&aether_alloc_test_armed, memory_order_relaxed)) return 0;
    if (t_alloc_fail_after <= 0) return 0;
    return --t_alloc_fail_after == 0;
}

void aether_alloc_failed(int why) {
    AetherFrames* F = aether_frames_here();
    if (F->exec_depth <= F->exec_floor) return;   /* none of this code's (#3392) */
    AetherExecRecord* x = &F->exec[F->exec_depth - 1];
    if (!x->stop) x->stop = why;
    caps_arm();
}

/* ------------------------------------------------------------------
 * Execution accounts (#2954)
 * ------------------------------------------------------------------
 * A budget the host owns, for executions that outlive one call: a script
 * that yields to its host and resumes later, while other scripts run on the
 * same thread in between. The account in use on a thread (aether_exec_use)
 * is charged every cap-aware allocation made there, and its parent chain
 * with it; an allocation that would take any account on the chain past its
 * allowance is refused, as the process cap refuses one, and inside an
 * execution the call then ends with AETHER_MEMORY_CAP (aether_alloc_failed).
 *
 * A block remembers the account it was charged to, so a free credits that
 * account wherever it happens: in another execution, under another account,
 * or in the host outside any. The blocks of every account are in one table,
 * kept only while some account exists (CAPS_SLOW_ACCOUNTS), behind one lock
 * with the accounts' parent links and the list of live accounts. A block
 * allocated under no account is in no table and credits nobody. */

static atomic_flag g_acct_lock = ATOMIC_FLAG_INIT;
static AetherExecAccount* g_live_accounts = NULL;
static int g_live_count = 0;

typedef struct { const void* p; AetherExecAccount* a; } AcctSlot;
static AcctSlot* g_slots = NULL;   /* open addressing, linear probes */
static size_t g_slot_cap = 0;      /* a power of two, or 0 */
static size_t g_slot_used = 0;

/* Held for a table probe or a chain walk, and for a free's one pass over
 * the table. A waiter spins a little, pausing, then gives up the processor
 * rather than spin hot (#3410). */
static inline void acct_yield(void) {
#if defined(AETHER_NO_THREADING)
    AETHER_CPU_PAUSE();
#elif defined(_WIN32)
    SwitchToThread();
#else
    sched_yield();
#endif
}

static inline void acct_lock(void) {
    unsigned spins = 0;
    while (atomic_flag_test_and_set_explicit(&g_acct_lock, memory_order_acquire)) {
        if (++spins < 64) AETHER_CPU_PAUSE();
        else acct_yield();
    }
}
static inline void acct_unlock(void) {
    atomic_flag_clear_explicit(&g_acct_lock, memory_order_release);
}

static inline size_t slot_of(const void* p, size_t cap) {
    uint64_t h = (uint64_t)(uintptr_t)p;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (size_t)h & (cap - 1);
}

/* The table's own memory is plain malloc: it is no account's and no cap's. */
static int slots_grow(void) {
    size_t cap = g_slot_cap ? g_slot_cap * 2 : 1024;
    AcctSlot* fresh = (AcctSlot*)calloc(cap, sizeof(AcctSlot));
    if (!fresh) return 0;
    for (size_t i = 0; i < g_slot_cap; i++) {
        if (!g_slots[i].p) continue;
        size_t s = slot_of(g_slots[i].p, cap);
        while (fresh[s].p) s = (s + 1) & (cap - 1);
        fresh[s] = g_slots[i];
    }
    free(g_slots);
    g_slots = fresh;
    g_slot_cap = cap;
    return 1;
}

/* Which addresses may have a block in the table (#3410): one bit per hash
 * of an address. A thread with no account in use that frees or allocates a
 * block whose bit is clear asks nothing more, and takes no lock.
 *
 * Why a clear bit is enough. A block is recorded only under the lock, and
 * its bit is set first (a release), before the allocator returns the
 * block to its caller. Whatever thread frees it got the pointer from that
 * caller, after that return (the program's own hand-over orders the two),
 * so its load (an acquire) sees the bit. A free that sees the bit clear is
 * of a block no account was charged for. The bits are cleared only under
 * the lock, when the table holds no block at all and enough of them are
 * set to be worth it: no address is in the table then, and a block
 * recorded after sets its bit again before it is handed out.
 *
 * What a false positive costs. A free (or an allocation with no account in
 * use) whose bit another recorded block set takes the lock and finds no
 * entry: one table probe under the lock. With n blocks recorded, about
 * n / 2^18 of such frees take it (20,000 blocks: 8%).
 *
 * 32 KiB, the owning copy's only. */
#define ACCT_FILTER_BITS  (1u << 18)
#define ACCT_FILTER_WORDS (ACCT_FILTER_BITS / 64)
static _Atomic uint64_t g_filter[ACCT_FILTER_WORDS];
static size_t g_filter_set = 0;   /* bits set, under the lock */

static inline uint32_t filter_bit(const void* p) {
    return (uint32_t)(((uint64_t)(uintptr_t)p * 0x9e3779b97f4a7c15ULL) >> (64 - 18));
}

static inline int filter_may_hold(const void* p) {
    uint32_t b = filter_bit(p);
    return (int)((atomic_load_explicit(&g_filter[b >> 6], memory_order_acquire) >> (b & 63)) & 1u);
}

/* Under the lock. */
static inline void filter_note(const void* p) {
    uint32_t b = filter_bit(p);
    uint64_t bit = (uint64_t)1 << (b & 63);
    if (!(atomic_fetch_or_explicit(&g_filter[b >> 6], bit, memory_order_release) & bit)) g_filter_set++;
}

/* Under the lock, with no block in the table. */
static void filter_clear(void) {
    for (size_t i = 0; i < ACCT_FILTER_WORDS; i++) atomic_store_explicit(&g_filter[i], 0, memory_order_relaxed);
    g_filter_set = 0;
}

/* Under the lock. */
static int slot_put(const void* p, AetherExecAccount* a) {
    if ((g_slot_used + 1) * 2 > g_slot_cap && !slots_grow()) return 0;
    size_t s = slot_of(p, g_slot_cap);
    while (g_slots[s].p && g_slots[s].p != p) s = (s + 1) & (g_slot_cap - 1);
    if (!g_slots[s].p) {
        filter_note(p);
        g_slot_used++;
    }
    g_slots[s].p = p;
    g_slots[s].a = a;
    return 1;
}

/* Under the lock: the slot of `p` emptied (the probe chain after it shifted
 * back), and the account it named returned. */
static AetherExecAccount* slot_take(const void* p) {
    if (!g_slot_cap) return NULL;
    size_t s = slot_of(p, g_slot_cap);
    while (g_slots[s].p && g_slots[s].p != p) s = (s + 1) & (g_slot_cap - 1);
    if (!g_slots[s].p) return NULL;
    AetherExecAccount* a = g_slots[s].a;
    g_slots[s].p = NULL;
    if (--g_slot_used == 0 && g_filter_set > ACCT_FILTER_BITS / 8) filter_clear();
    size_t hole = s;
    for (size_t j = (s + 1) & (g_slot_cap - 1); g_slots[j].p; j = (j + 1) & (g_slot_cap - 1)) {
        size_t home = slot_of(g_slots[j].p, g_slot_cap);
        /* Moves back when its home is not in (hole, j] cyclically. */
        int stays = hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
        if (!stays) {
            g_slots[hole] = g_slots[j];
            g_slots[j].p = NULL;
            hole = j;
        }
    }
    return a;
}

/* Under the lock: `bytes` charged to `a` and its ancestors, or to none of
 * them when one would pass its allowance (which is then marked tripped). */
static int chain_charge(AetherExecAccount* a, uint64_t bytes) {
    for (AetherExecAccount* x = a; x; x = x->parent) {
        uint64_t cur = atomic_load_explicit(&x->used, memory_order_relaxed);
        if (x->memory_bytes && (cur + bytes > x->memory_bytes || cur + bytes < cur)) {
            for (AetherExecAccount* y = a; y != x; y = y->parent)
                atomic_fetch_sub_explicit(&y->used, bytes, memory_order_relaxed);
            atomic_fetch_or_explicit(&x->tripped, AETHER_ACCOUNT_TRIPPED_MEMORY, memory_order_relaxed);
            return 0;
        }
        atomic_store_explicit(&x->used, cur + bytes, memory_order_relaxed);
    }
    return 1;
}

static void chain_credit(AetherExecAccount* a, uint64_t bytes) {
    for (AetherExecAccount* x = a; x; x = x->parent) {
        uint64_t cur = atomic_load_explicit(&x->used, memory_order_relaxed);
        atomic_store_explicit(&x->used, cur >= bytes ? cur - bytes : 0, memory_order_relaxed);
    }
}

/* The allocation fast-path word (aether_caps_slow) of every copy that shares
 * this copy's accounts, its own first: CAPS_SLOW_ACCOUNTS is set in each
 * while an account exists, so a copy's fast path stays one load of its own
 * word. Every copy registers at its first execution or account call, used
 * or not: a library's copy allocates under the account a host put in use
 * through another copy, so it must know when one exists. A copy leaves when
 * it is unloaded (#3411), so its word is never written after its image is
 * gone. The list grows as needed (plain malloc); a copy that cannot join it
 * keeps its own word slow and always asks the accounts. Under the lock. */
static _Atomic unsigned* g_copy_words_here[16] = { &aether_caps_slow };
static _Atomic unsigned** g_copy_words = g_copy_words_here;
static int g_copy_count = 1;
static int g_copy_cap = 16;

static void copies_mark(int on) {
    for (int i = 0; i < g_copy_count; i++) {
        if (on) atomic_fetch_or_explicit(g_copy_words[i], CAPS_SLOW_ACCOUNTS, memory_order_release);
        else atomic_fetch_and_explicit(g_copy_words[i], ~CAPS_SLOW_ACCOUNTS, memory_order_release);
    }
}

static int own_register(_Atomic unsigned* word) {
    acct_lock();
    for (int i = 0; i < g_copy_count; i++) {
        if (g_copy_words[i] == word) { acct_unlock(); return 0; }
    }
    if (g_copy_count == g_copy_cap) {
        int cap = g_copy_cap * 2;
        _Atomic unsigned** more = (_Atomic unsigned**)malloc(sizeof(*more) * (size_t)cap);
        if (!more) { acct_unlock(); return -1; }
        memcpy(more, g_copy_words, sizeof(*more) * (size_t)g_copy_count);
        if (g_copy_words != g_copy_words_here) free(g_copy_words);
        g_copy_words = more;
        g_copy_cap = cap;
    }
    g_copy_words[g_copy_count++] = word;
    if (g_live_count > 0) atomic_fetch_or_explicit(word, CAPS_SLOW_ACCOUNTS, memory_order_release);
    acct_unlock();
    return 0;
}

/* A copy is unloaded, or the process ends (#3411): its word leaves. At
 * process exit on Windows the other threads are gone first, and one may
 * have held the lock: the wait is bounded, and past it the word stays
 * (nothing marks one at exit). */
static void own_unregister(_Atomic unsigned* word) {
    int got = 0;
    for (int tries = 0; tries < 100000; tries++) {
        if (!atomic_flag_test_and_set_explicit(&g_acct_lock, memory_order_acquire)) { got = 1; break; }
        acct_yield();
    }
    if (!got) return;
    for (int i = 1; i < g_copy_count; i++) {
        if (g_copy_words[i] == word) {
            g_copy_words[i] = g_copy_words[--g_copy_count];
            break;
        }
    }
    acct_unlock();
}

/* Under the lock: one thing less names `a`. 1 when it was the last, and the
 * caller frees the block once it has let go of the lock. */
static int acct_unref(AetherExecAccount* a) {
    return --a->refs == 0;
}

/* Under the lock. The account in use on this thread becomes `a`: `a` taken
 * for this thread, the one it replaces let go (and returned in `*gone` when
 * nothing names it any more). 0, or -1 when `a` is in use on another
 * thread (the account in use is then left as it was). */
static int account_switch(AetherFrames* F, AetherExecAccount* a, AetherExecAccount** gone) {
    AetherExecAccount* was = F->account;
    if (a == was) return 0;
    if (a) {
        uintptr_t expected = 0;
        if (!atomic_compare_exchange_strong_explicit(&a->thread, &expected, (uintptr_t)F,
                                                     memory_order_acq_rel, memory_order_relaxed))
            return -1;
        a->refs++;
    }
    F->account = a;
    if (was) {
        atomic_store_explicit(&was->thread, 0, memory_order_release);
        if (acct_unref(was)) *gone = was;
    }
    return 0;
}

/* The account in use on this thread. One the host freed meanwhile (on
 * another thread, while it was in use here) is let go here, the thread
 * whose it was: the next allocation, call or switch here finds it gone. */
static AetherExecAccount* own_here(void) {
    AetherFrames* F = aether_frames_here();
    AetherExecAccount* a = F->account;
    if (!a || !atomic_load_explicit(&a->dead, memory_order_acquire)) return a;
    AetherExecAccount* gone = NULL;
    acct_lock();
    if (F->account && atomic_load_explicit(&F->account->dead, memory_order_relaxed))
        (void)account_switch(F, NULL, &gone);
    a = F->account;
    acct_unlock();
    free(gone);
    return a;
}

/* An execution begins: the account in use, held until it ends. */
static AetherExecAccount* own_hold(void) {
    AetherExecAccount* a = own_here();
    if (!a) return NULL;
    acct_lock();
    if (atomic_load_explicit(&a->dead, memory_order_relaxed)) a = NULL;
    else a->refs++;
    acct_unlock();
    return a;
}

int64_t aether_caps_now_ns(void) {
    return now_ns();
}

/* The ACTIVE accounts this thread is timing (#3409), innermost last: each
 * since the start of the outermost execution open here under it or under a
 * descendant of it, and held until that execution ends. An execution nested
 * inside one that already times an account adds no time of its own to it,
 * so time spent in executions under an account is charged once, however
 * they nest, across runtime copies too: they all time through the copy
 * whose accounts they share. The first ACCT_TIMED_INLINE are kept here, the
 * rest in a block that grows as needed (plain malloc, no account's), freed
 * when this thread times none. Under the lock, this thread's only. */
#define ACCT_TIMED_INLINE 8
typedef struct { AetherExecAccount* a; int64_t since; } AcctTimed;
static AETHER_TLS_SHARED AcctTimed t_timed_inline[ACCT_TIMED_INLINE];
static AETHER_TLS_SHARED AcctTimed* t_timed_more;   /* entries past the inline ones */
static AETHER_TLS_SHARED int t_timed_more_cap;
static AETHER_TLS_SHARED int t_timed_n;

static inline AcctTimed* timed_at(int i) {
    return i < ACCT_TIMED_INLINE ? &t_timed_inline[i] : &t_timed_more[i - ACCT_TIMED_INLINE];
}

/* Under the lock: when this thread began timing `a`, or -1. */
static int64_t timed_since(const AetherExecAccount* a) {
    for (int i = t_timed_n - 1; i >= 0; i--)
        if (timed_at(i)->a == a) return timed_at(i)->since;
    return -1;
}

/* Under the lock: `a` timed from `now` (and held). 0 when there was no room
 * and none could be had. */
static int timed_push(AetherExecAccount* a, int64_t now) {
    int i = t_timed_n;
    if (i >= ACCT_TIMED_INLINE && i - ACCT_TIMED_INLINE >= t_timed_more_cap) {
        int cap = t_timed_more_cap ? t_timed_more_cap * 2 : ACCT_TIMED_INLINE;
        /* A checked allocation: the test hook refuses it as it does any. */
        if (aether_alloc_test_refuses()) return 0;
        AcctTimed* more = (AcctTimed*)realloc(t_timed_more, sizeof(AcctTimed) * (size_t)cap);
        if (!more) return 0;
        t_timed_more = more;
        t_timed_more_cap = cap;
    }
    timed_at(i)->a = a;
    timed_at(i)->since = now;
    t_timed_n = i + 1;
    a->refs++;
    return 1;
}

/* An execution begins under `a` (held for it): `*at` the deadline in force
 * for it, the earliest of `a`'s and its ancestors' as absolute monotonic ns
 * (0: none), `*tripped` set when one already passed. The ACTIVE accounts on
 * the chain no execution open here times yet are timed from now, for this
 * one: `*timed` says how many, for its end. -1 when one of them could not
 * be timed (no memory for the thread's list): the execution must not run,
 * or its account's budget would go unenforced. */
static int own_exec_begin(AetherExecAccount* a, int64_t* at_out, int* tripped, int* timed) {
    int64_t at = 0, now = now_ns();
    int tr = 0, n = 0, ok = 1;
    acct_lock();
    /* A freed account sets no deadline: its chain is unlinked. */
    if (a && atomic_load_explicit(&a->dead, memory_order_relaxed)) a = NULL;
    for (AetherExecAccount* x = a; x; x = x->parent) {
        int64_t own = 0;
        if (x->deadline_counts == AETHER_DEADLINE_ACTIVE) {
            int64_t since = timed_since(x);
            if (since < 0) {
                if (!timed_push(x, now)) { ok = 0; break; }
                n++;
                since = now;
            }
            /* What is left of it: its budget, less what it spent before and
             * since this thread began timing it. */
            if (x->active_budget_ns)
                own = since + x->active_budget_ns - atomic_load_explicit(&x->active_spent_ns, memory_order_relaxed);
        } else {
            own = x->deadline_at_ns;
        }
        if (atomic_load_explicit(&x->tripped, memory_order_relaxed) & AETHER_ACCOUNT_TRIPPED_DEADLINE) tr = 1;
        if (own && (!at || own < at)) at = own;
    }
    acct_unlock();
    if (at && at <= now) tr = 1;
    *at_out = at;
    *tripped = tr;
    *timed = n;
    return ok ? 0 : -1;
}

/* The execution begun under `a` ends, its deadline tripped or not: the
 * accounts it began timing are charged the time since, and let go; the
 * trip, where a deadline on the chain passed, recorded. */
static void own_exec_end(AetherExecAccount* a, int timed, int tripped) {
    int64_t now = now_ns();
    AetherExecAccount* gone = NULL;   /* let go for the last time, freed after the lock */
    acct_lock();
    for (int i = 0; i < timed && t_timed_n > 0; i++) {
        AcctTimed e = *timed_at(--t_timed_n);
        AetherExecAccount* x = e.a;
        /* Nothing is charged to an account the host freed meanwhile. */
        if (!atomic_load_explicit(&x->dead, memory_order_relaxed)) {
            int64_t spent = atomic_fetch_add_explicit(&x->active_spent_ns, now - e.since,
                                                      memory_order_relaxed) + (now - e.since);
            if (tripped && x->active_budget_ns && spent >= x->active_budget_ns)
                atomic_fetch_or_explicit(&x->tripped, AETHER_ACCOUNT_TRIPPED_DEADLINE, memory_order_relaxed);
        }
        if (acct_unref(x)) {   /* dead and unlinked: its live-list link is free */
            x->next_live = gone;
            gone = x;
        }
    }
    if (t_timed_n == 0 && t_timed_more) {
        free(t_timed_more);
        t_timed_more = NULL;
        t_timed_more_cap = 0;
    }
    /* An account trips on its own deadline: the call's trip flag is only the
     * sign that a safe point saw one pass. The ACTIVE ones an enclosing
     * execution times count the time open so far. */
    if (tripped && a && !atomic_load_explicit(&a->dead, memory_order_relaxed)) {
        for (AetherExecAccount* x = a; x; x = x->parent) {
            int own = 0;
            if (x->deadline_counts == AETHER_DEADLINE_ACTIVE) {
                int64_t since = timed_since(x);
                own = since >= 0 && x->active_budget_ns &&
                      atomic_load_explicit(&x->active_spent_ns, memory_order_relaxed) + (now - since) >=
                          x->active_budget_ns;
            } else {
                own = x->deadline_at_ns && now >= x->deadline_at_ns;
            }
            if (own) atomic_fetch_or_explicit(&x->tripped, AETHER_ACCOUNT_TRIPPED_DEADLINE, memory_order_relaxed);
        }
    }
    acct_unlock();
    while (gone) {
        AetherExecAccount* next = gone->next_live;
        free(gone);
        gone = next;
    }
}

static AetherExecAccount* own_new(const AetherExecLimits* limits) {
    AetherExecAccount* a = (AetherExecAccount*)calloc(1, sizeof(AetherExecAccount));
    if (!a) return NULL;
    a->owner = &g_acct_lock;
    if (limits) {
        a->memory_bytes = limits->memory_bytes;
        a->deadline_counts = limits->deadline_counts == AETHER_DEADLINE_ACTIVE
                                 ? AETHER_DEADLINE_ACTIVE : AETHER_DEADLINE_ELAPSED;
        if (limits->deadline_ms > 0) {
            if (a->deadline_counts == AETHER_DEADLINE_ACTIVE)
                a->active_budget_ns = ms_to_ns(limits->deadline_ms);
            else
                a->deadline_at_ns = ns_after_ms(limits->deadline_ms);
            caps_arm();
        }
    }
    acct_lock();
    /* Only a live parent of these accounts is linked. One they do not hold
     * (the host freed it, or a copy that does not share them made it) is
     * refused, not dropped (#3412): the account would be wider than the
     * parent meant to narrow it. Found by its address alone, never read. */
    if (limits && limits->parent) {
        for (AetherExecAccount* x = g_live_accounts; x; x = x->next_live)
            if (x == limits->parent) { a->parent = x; break; }
        if (!a->parent) {
            acct_unlock();
            free(a);
            return NULL;
        }
    }
    a->refs = 1;   /* the live list's */
    a->next_live = g_live_accounts;
    g_live_accounts = a;
    g_live_count++;
    if (g_live_count == 1) copies_mark(1);
    acct_unlock();
    return a;
}

/* The host frees `a` (#3407): nothing in the runtime names it after this.
 * In use on this thread, it is in use no more. In use on another thread,
 * or named by an execution that began under it (here or there), it is
 * dead: charged, put in use and linked no more, and the block goes when
 * the last of them lets go of it. */
static void own_free(AetherExecAccount* a) {
    if (!a) return;
    acct_lock();
    AetherExecAccount** link = &g_live_accounts;
    while (*link && *link != a) link = &(*link)->next_live;
    if (!*link) { acct_unlock(); return; }   /* not a live account */
    *link = a->next_live;
    g_live_count--;
    atomic_store_explicit(&a->dead, 1, memory_order_release);
    /* Its blocks credit nobody from now on: they count only against the
     * process cap until they are freed. So its ancestors are credited now
     * with what its own live blocks were charged (#3408): its count, less
     * what its live children hold. Those stay charged where they are: a
     * child goes under its parent's parent, which its blocks were charged
     * to all along, and keeps narrowing under it. */
    uint64_t own = atomic_load_explicit(&a->used, memory_order_relaxed);
    for (AetherExecAccount* x = g_live_accounts; x; x = x->next_live) {
        if (x->parent != a) continue;
        uint64_t held = atomic_load_explicit(&x->used, memory_order_relaxed);
        own = own >= held ? own - held : 0;
        x->parent = a->parent;
    }
    if (a->parent && own) chain_credit(a->parent, own);
    a->parent = NULL;
    /* One pass (#3410): a take shifts later entries of its probe chain back
     * into the hole, so the slot is looked at again until it holds none of
     * its blocks. An entry shifted to a slot already passed comes from the
     * wrapped end of a chain, passed too, so it is none of them either. */
    for (size_t i = 0; g_live_count > 0 && i < g_slot_cap; i++)
        while (g_slots[i].p && g_slots[i].a == a) (void)slot_take(g_slots[i].p);
    if (g_live_count == 0) {   /* every block left was its: the table goes */
        free(g_slots);
        g_slots = NULL;
        g_slot_cap = 0;
        g_slot_used = 0;
        if (g_filter_set) filter_clear();
        copies_mark(0);
    }
    AetherExecAccount* gone = NULL;
    AetherFrames* F = aether_frames_here();
    if (F->account == a) (void)account_switch(F, NULL, &gone);
    if (acct_unref(a)) gone = a;   /* the live list's */
    acct_unlock();
    free(gone);
}

/* aether_exec_use: an account these accounts did not make (one another copy
 * of the runtime made, where the copies did not find each other) is refused,
 * so a copy never charges, links or records a block for an account another
 * copy's lock guards and another copy may free; so is one the host freed.
 * The one it replaces is given back unless the host freed it. */
static int own_use(AetherExecAccount* a, AetherExecAccount** previous) {
    if (a && a->owner != (const void*)&g_acct_lock) return -1;
    AetherFrames* F = aether_frames_here();
    AetherExecAccount* gone = NULL;
    acct_lock();
    if (a && atomic_load_explicit(&a->dead, memory_order_relaxed)) { acct_unlock(); return -1; }
    AetherExecAccount* was = F->account;
    if (was && atomic_load_explicit(&was->dead, memory_order_relaxed)) was = NULL;
    int r = account_switch(F, a, &gone);
    acct_unlock();
    free(gone);
    if (r != 0) return -1;
    if (previous) *previous = was;
    return 0;
}

/* An execution ends: the account in use when it began is in use again,
 * whatever the code it ran left (#2954), unless the host freed it, and the
 * execution lets go of it. One another thread has taken meanwhile is not
 * taken from it: then no account is in use. */
static void own_restore(AetherExecAccount* a) {
    AetherFrames* F = aether_frames_here();
    AetherExecAccount* gone[2] = { NULL, NULL };
    acct_lock();
    AetherExecAccount* back = a && !atomic_load_explicit(&a->dead, memory_order_relaxed) ? a : NULL;
    if (account_switch(F, back, &gone[0]) != 0) (void)account_switch(F, NULL, &gone[0]);
    if (a && acct_unref(a)) gone[1] = a;   /* the execution's */
    acct_unlock();
    free(gone[0]);
    if (gone[1] != gone[0]) free(gone[1]);
}

/* An actor step begins (aether_step_barrier.h): the account in use is the
 * code's the step runs inside, not the step's, whose allocations an actor's
 * own thread would not charge to it. Put aside, still this thread's, so no
 * other thread takes it meanwhile; NULL when none was in use. */
static AetherExecAccount* own_step_aside(void) {
    AetherFrames* F = aether_frames_here();
    AetherExecAccount* was = F->account;
    F->account = NULL;
    return was;
}

/* The step ends: one it left in use is let go, and the one put aside is in
 * use again, or let go too when the host freed it meanwhile. */
static void own_step_back(AetherExecAccount* was) {
    AetherFrames* F = aether_frames_here();
    if (!F->account && !was) return;
    AetherExecAccount* gone[2] = { NULL, NULL };
    acct_lock();
    if (F->account && F->account != was) (void)account_switch(F, NULL, &gone[0]);
    F->account = was;
    if (was && atomic_load_explicit(&was->dead, memory_order_relaxed))
        (void)account_switch(F, NULL, &gone[1]);
    acct_unlock();
    free(gone[0]);
    free(gone[1]);
}

static uint64_t own_used(const AetherExecAccount* a) {
    return a ? atomic_load_explicit(&a->used, memory_order_relaxed) : 0;
}

static int own_tripped(const AetherExecAccount* a) {
    return a ? atomic_load_explicit(&a->tripped, memory_order_relaxed) : 0;
}

/* The account in use's charge for a block of `bytes`, under the lock: 1 and
 * the block recorded (when an account is in use), or 0 refused. With no
 * account in use the block is in no table: a stale entry at its address
 * (a block freed with plain free()) is dropped. */
static int own_alloc(void* p, size_t bytes) {
    AetherExecAccount* a = own_here();
    /* No account in use here: nothing to charge, and a stale entry at this
     * address (a block an account was charged for, freed with plain free())
     * only where its bit is set. No lock otherwise (#3410). */
    if (!a && !filter_may_hold(p)) return 1;
    acct_lock();
    if (a && atomic_load_explicit(&a->dead, memory_order_relaxed)) a = NULL;   /* freed meanwhile */
    if (!a) {
        (void)slot_take(p);
        acct_unlock();
        return 1;
    }
    int ok = chain_charge(a, (uint64_t)bytes);
    if (ok && !slot_put(p, a)) {
        chain_credit(a, (uint64_t)bytes);   /* no room to remember it: charged to none */
    }
    acct_unlock();
    return ok;
}

static void own_free_block(const void* p, size_t bytes) {
    if (!filter_may_hold(p)) return;   /* no account was charged for it: no lock */
    acct_lock();
    AetherExecAccount* a = slot_take(p);
    if (a) chain_credit(a, (uint64_t)bytes);
    acct_unlock();
}

/* A realloc of a cap-aware block, for the accounts: the old block's bytes
 * back to the account it was charged to, the new block's charged to the
 * account in use, before the realloc, so a refusal leaves it as it was. 0
 * refused. own_realloc_end records the new block, or undoes the charge
 * when the realloc itself failed (np NULL). Both accounts are held from one
 * to the other, so a free meanwhile leaves neither naming a freed block. */
static int own_realloc_begin(void* p, size_t old_bytes, size_t new_bytes,
                             AetherExecAccount** old_owner, AetherExecAccount** now) {
    *now = own_here();
    if (!*now && (!p || !filter_may_hold(p))) {   /* nothing to charge or credit: no lock */
        *old_owner = NULL;
        return 1;
    }
    acct_lock();
    if (*now && atomic_load_explicit(&(*now)->dead, memory_order_relaxed)) *now = NULL;
    *old_owner = p ? slot_take(p) : NULL;
    if (*old_owner) chain_credit(*old_owner, (uint64_t)old_bytes);
    if (*now && !chain_charge(*now, (uint64_t)new_bytes)) {
        if (*old_owner) {
            (void)chain_charge(*old_owner, (uint64_t)old_bytes);   /* it was within it */
            (void)slot_put(p, *old_owner);
        }
        acct_unlock();
        return 0;
    }
    if (*old_owner) (*old_owner)->refs++;
    if (*now) (*now)->refs++;
    acct_unlock();
    return 1;
}

static void own_realloc_end(void* p, void* np, size_t old_bytes, size_t new_bytes,
                            AetherExecAccount* old_owner, AetherExecAccount* now) {
    if (!old_owner && !now && !(np && filter_may_hold(np))) return;   /* nothing held: no lock */
    AetherExecAccount* gone[2] = { NULL, NULL };
    acct_lock();
    AetherExecAccount* was = old_owner && !atomic_load_explicit(&old_owner->dead, memory_order_relaxed)
                                 ? old_owner : NULL;
    AetherExecAccount* in_use = now && !atomic_load_explicit(&now->dead, memory_order_relaxed) ? now : NULL;
    if (!np) {
        if (in_use) chain_credit(in_use, (uint64_t)new_bytes);
        if (was) {
            (void)chain_charge(was, (uint64_t)old_bytes);
            (void)slot_put(p, was);
        }
    } else if (in_use) {
        if (!slot_put(np, in_use)) chain_credit(in_use, (uint64_t)new_bytes);
    } else {
        (void)slot_take(np);
    }
    if (old_owner && acct_unref(old_owner)) gone[0] = old_owner;
    if (now && acct_unref(now)) gone[1] = now;
    acct_unlock();
    free(gone[0]);
    if (gone[1] != gone[0]) free(gone[1]);
}

/* ------------------------------------------------------------------
 * The accounts are one per process (#2954)
 * ------------------------------------------------------------------
 * A library with a runtime of its own makes its allocations through its own
 * copy of this file, while the account in use was set through another (an
 * Aether host's std.host, or a C host's). So the accounts, the table of
 * blocks, the lock and each thread's account in use live in one copy, the
 * first loaded image with a record of them (aether_rendezvous_find, as the
 * sandbox levels do; an executable that exports nothing has its record,
 * #3445), and every
 * copy works through it: the lock is one per process, and an account in use
 * on one thread is refused on another whichever copy asks. A copy finds it
 * once, the first time it enters an execution or calls the account API, and
 * is given the accounts' bit in its own fast-path word from then on.
 *
 * The rendezvous carries a version and the sizes of what the copies pass to
 * each other. A copy whose layout differs refuses: it reports it once, its
 * account calls fail (aether_exec_account_new returns NULL, aether_exec_use
 * -1), its allocations are charged to no account, and it never reads or
 * writes the other copy's state. The copy that owns the accounts must stay
 * loaded while another uses them: unloading it first is not supported. */

#define AETHER_ACCOUNTS_RV_VERSION 1

typedef struct AetherAccountsRendezvous {
    uint32_t version;
    uint32_t size;            /* sizeof(AetherAccountsRendezvous) */
    uint32_t account_size;    /* sizeof(struct AetherExecAccount) */
    uint32_t limits_size;     /* sizeof(AetherExecLimits) */
    int (*register_copy)(_Atomic unsigned* fast_path_word);
    void (*unregister_copy)(_Atomic unsigned* fast_path_word);
    AetherExecAccount* (*account_new)(const AetherExecLimits* limits);
    void (*account_free)(AetherExecAccount* account);
    int (*use)(AetherExecAccount* account, AetherExecAccount** previous);
    void (*restore)(AetherExecAccount* account);
    AetherExecAccount* (*here)(void);
    AetherExecAccount* (*hold)(void);
    uint64_t (*used)(const AetherExecAccount* account);
    int (*tripped)(const AetherExecAccount* account);
    int (*exec_begin)(AetherExecAccount* account, int64_t* at, int* tripped, int* timed);
    void (*exec_end)(AetherExecAccount* account, int timed, int tripped);
    int (*alloc)(void* p, size_t bytes);
    void (*free_block)(const void* p, size_t bytes);
    int (*realloc_begin)(void* p, size_t old_bytes, size_t new_bytes,
                         AetherExecAccount** old_owner, AetherExecAccount** now);
    void (*realloc_end)(void* p, void* np, size_t old_bytes, size_t new_bytes,
                        AetherExecAccount* old_owner, AetherExecAccount* now);
    AetherExecAccount* (*step_aside)(void);
    void (*step_back)(AetherExecAccount* was);
} AetherAccountsRendezvous;

static const AetherAccountsRendezvous g_own_accounts = {
    AETHER_ACCOUNTS_RV_VERSION, (uint32_t)sizeof(AetherAccountsRendezvous),
    (uint32_t)sizeof(struct AetherExecAccount), (uint32_t)sizeof(AetherExecLimits),
    own_register, own_unregister, own_new, own_free, own_use, own_restore, own_here, own_hold,
    own_used, own_tripped,
    own_exec_begin, own_exec_end, own_alloc, own_free_block, own_realloc_begin, own_realloc_end,
    own_step_aside, own_step_back,
};

/* This copy's, which its record names (aether_rv_mark.h, #3445): the other
 * copies find it by the record where records are read, and by name where
 * not (AETHER_RV_FN). */
AETHER_RV_FN const AetherAccountsRendezvous* aether_accounts_rendezvous(void) {
    return &g_own_accounts;
}

#if AETHER_RV_MARKS
AETHER_RV_MARK(g_accounts_mark, AETHER_RV_KIND_ACCOUNTS, aether_accounts_rendezvous);
#  define ACCOUNTS_SELF (&g_accounts_mark)
#else
#  define ACCOUNTS_SELF NULL
#endif

/* A copy whose layout differs from the owning copy's: no account, ever. */
static int refused_register(_Atomic unsigned* w) { (void)w; return -1; }
static void refused_unregister(_Atomic unsigned* w) { (void)w; }
static AetherExecAccount* refused_new(const AetherExecLimits* l) { (void)l; return NULL; }
static void refused_free(AetherExecAccount* a) { (void)a; }
static int refused_use(AetherExecAccount* a, AetherExecAccount** prev) {
    if (prev) *prev = NULL;
    return a ? -1 : 0;
}
static void refused_restore(AetherExecAccount* a) { (void)a; }
static AetherExecAccount* refused_here(void) { return NULL; }
static uint64_t refused_used(const AetherExecAccount* a) { (void)a; return 0; }
static int refused_tripped(const AetherExecAccount* a) { (void)a; return 0; }
static int refused_begin(AetherExecAccount* a, int64_t* at, int* tr, int* n) {
    (void)a;
    *at = 0;
    *tr = 0;
    *n = 0;
    return 0;
}
static void refused_end(AetherExecAccount* a, int n, int t) { (void)a; (void)n; (void)t; }
static int refused_alloc(void* p, size_t n) { (void)p; (void)n; return 1; }
static void refused_free_block(const void* p, size_t n) { (void)p; (void)n; }
static int refused_realloc_begin(void* p, size_t o, size_t n, AetherExecAccount** oo, AetherExecAccount** now) {
    (void)p; (void)o; (void)n; *oo = NULL; *now = NULL;
    return 1;
}
static void refused_realloc_end(void* p, void* np, size_t o, size_t n, AetherExecAccount* oo, AetherExecAccount* now) {
    (void)p; (void)np; (void)o; (void)n; (void)oo; (void)now;
}
static AetherExecAccount* refused_step_aside(void) { return NULL; }
static void refused_step_back(AetherExecAccount* was) { (void)was; }
static const AetherAccountsRendezvous g_refused_accounts = {
    AETHER_ACCOUNTS_RV_VERSION, (uint32_t)sizeof(AetherAccountsRendezvous),
    (uint32_t)sizeof(struct AetherExecAccount), (uint32_t)sizeof(AetherExecLimits),
    refused_register, refused_unregister, refused_new, refused_free, refused_use, refused_restore,
    refused_here, refused_here,
    refused_used, refused_tripped, refused_begin, refused_end, refused_alloc, refused_free_block,
    refused_realloc_begin, refused_realloc_end, refused_step_aside, refused_step_back,
};

static const AetherAccountsRendezvous* _Atomic g_accounts = NULL;
_Atomic int aether_accounts_found = 0;

typedef const AetherAccountsRendezvous* (*accounts_rv_fn)(void);

static void accounts_copy_unloads(void);

/* The accounts this copy works through, found the first time. */
static const AetherAccountsRendezvous* accounts(void) {
    const AetherAccountsRendezvous* r = atomic_load_explicit(&g_accounts, memory_order_acquire);
    if (r) return r;
    const AetherAccountsRendezvous* found = &g_own_accounts;
    void* p = aether_rendezvous_find("aether_accounts_rendezvous", AETHER_RV_KIND_ACCOUNTS, ACCOUNTS_SELF);
    if (p) {
        accounts_rv_fn f;
        memcpy(&f, &p, sizeof(f));
        const AetherAccountsRendezvous* other = f();
        if (other && other != &g_own_accounts) {
            /* The version and size first: only those are read before it
             * is known the rest is laid out as this copy lays it out. */
            if (other->version == AETHER_ACCOUNTS_RV_VERSION &&
                other->size == (uint32_t)sizeof(AetherAccountsRendezvous) &&
                other->account_size == (uint32_t)sizeof(struct AetherExecAccount) &&
                other->limits_size == (uint32_t)sizeof(AetherExecLimits)) {
                found = other;
            } else {
                fprintf(stderr,
                        "aether: this runtime copy cannot share execution accounts with the one "
                        "loaded first (accounts version %u, %u-byte table, against %u, %u): its "
                        "account calls fail and its allocations are charged to no account; build "
                        "the libraries with one Aether version, or with --shared-runtime\n",
                        (unsigned)AETHER_ACCOUNTS_RV_VERSION, (unsigned)sizeof(AetherAccountsRendezvous),
                        (unsigned)other->version, (unsigned)other->size);
                found = &g_refused_accounts;
            }
        }
    }
    const AetherAccountsRendezvous* expected = NULL;
    if (atomic_compare_exchange_strong_explicit(&g_accounts, &expected, found,
                                                memory_order_acq_rel, memory_order_acquire)) {
        /* One that cannot join the list (no memory, #3411) asks the
         * accounts on every allocation rather than miss one in use. */
        if (found->register_copy(&aether_caps_slow) != 0 && found != &g_refused_accounts)
            atomic_fetch_or_explicit(&aether_caps_slow, CAPS_SLOW_ACCOUNTS, memory_order_release);
#if !(defined(__GNUC__) || defined(__clang__))
        if (found != &g_own_accounts) atexit(accounts_copy_unloads);
#endif
    }
    atomic_store_explicit(&aether_accounts_found, 1, memory_order_release);
    return atomic_load_explicit(&g_accounts, memory_order_acquire);
}

/* This copy is unloaded, or the process ends (#3411): its word leaves the
 * accounts of the copy it shares them with, which would otherwise write
 * into its image once it is gone. Run as a destructor (with atexit where
 * there is none, which a library's C runtime runs at its unload). */
static void accounts_copy_unloads(void) {
    const AetherAccountsRendezvous* r = atomic_load_explicit(&g_accounts, memory_order_acquire);
    if (r && r != &g_own_accounts) r->unregister_copy(&aether_caps_slow);
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((destructor)) static void accounts_copy_dtor(void) {
    accounts_copy_unloads();
}
#endif

void aether_accounts_register(void) {
    (void)accounts();
}

AetherExecAccount* aether_account_here(void) {
    return accounts()->here();
}

AetherExecAccount* aether_account_hold(void) {
    return accounts()->hold();
}

int aether_account_exec_begin(AetherExecAccount* a, int64_t* at, int* tripped, int* timed) {
    return accounts()->exec_begin(a, at, tripped, timed);
}

void aether_account_exec_end(AetherExecAccount* a, int timed, int tripped) {
    accounts()->exec_end(a, timed, tripped);
}

AetherExecAccount* aether_account_step_aside(void) {
    return accounts()->step_aside();
}

void aether_account_step_back(AetherExecAccount* was) {
    accounts()->step_back(was);
}

void aether_account_restore(AetherExecAccount* a) {
    accounts()->restore(a);
}

int aether_accounts_in_force(void) {
    return (atomic_load_explicit(&aether_caps_slow, memory_order_relaxed) & CAPS_SLOW_ACCOUNTS) != 0;
}

AETHER_CAPS_API AetherExecAccount* aether_exec_account_new(const AetherExecLimits* limits) {
    return accounts()->account_new(limits);
}

AETHER_CAPS_API void aether_exec_account_free(AetherExecAccount* account) {
    accounts()->account_free(account);
}

AETHER_CAPS_API int aether_exec_use(AetherExecAccount* account, AetherExecAccount** previous) {
    return accounts()->use(account, previous);
}

AETHER_CAPS_API uint64_t aether_exec_account_used(const AetherExecAccount* account) {
    return accounts()->used(account);
}

AETHER_CAPS_API int aether_exec_account_tripped(const AetherExecAccount* account) {
    return accounts()->tripped(account);
}

/* std.host's: an account from scalars, for a caller with no C struct. */
AETHER_CAPS_API AetherExecAccount* aether_exec_account_make(uint64_t memory_bytes, int64_t deadline_ms,
                                                            int counts, AetherExecAccount* parent) {
    AetherExecLimits l = { memory_bytes, deadline_ms, counts, parent };
    return aether_exec_account_new(&l);
}

/* ------------------------------------------------------------------
 * Runtime copies that share state (#2955, #2954)
 * ------------------------------------------------------------------
 * A library built without --shared-runtime carries a copy of the runtime,
 * and on Windows and macOS that copy stays apart from the program's. State
 * that must be one per process (the sandbox levels a thread is inside, the
 * execution accounts) lives in one copy, the first loaded image, in load
 * order, with a record of it (aether_rv_mark.h): every copy asks the same
 * question, so they agree, whatever the images export (#3445). A record
 * counts only when the function it names lies inside its image.
 *
 * The walk stops at the first record, and at the asking copy's own image
 * whether its record is read or not, so it reads no image loaded after that
 * one. Those before it are loaded whole, relocated: the loader loads one
 * library at a time (glibc's and musl's load lock, dyld's, the Windows
 * loader lock), adds each at the end of the list, and relocates what one
 * load brings before any of its constructors runs. So no copy waits for a
 * load or tests an image by name, and every copy answers alike at any
 * moment. An image is held, or read under the loader's lock, while it is
 * read; the copy that owns the state must stay loaded while another uses
 * it. A namespace of its own (glibc's dlmopen) is a process of its own here:
 * its copies find none outside it.
 * ------------------------------------------------------------------ */

#if AETHER_RV_MARKS
#  if AETHER_RV_ELF
#    define RV_DESC_AT offsetof(AetherRvMark, fn_offset)
#    define RV_DESC_SIZE (2 * sizeof(int32_t))
#  else
#    define RV_DESC_AT offsetof(AetherRvMark, fn)
#    define RV_DESC_SIZE sizeof(aether_rv_fn)
#  endif

/* The record of `kind` in [p, p + size), or NULL: its 24-byte head (the note
 * header and the name, aether_rv_mark.h) at any 4-aligned place. Not by
 * walking the notes: a linker that puts notes of both alignments in one
 * PT_NOTE (gold), or starts it 4 past an 8-aligned place (after .interp),
 * leaves a walk by the segment's alignment out of step with the record. The
 * head is 24 exact bytes, and what it names must lie inside its image, which
 * the callers check. */
static const AetherRvMark* rv_mark_in(const unsigned char* p, size_t size, uint32_t kind) {
    unsigned char head[RV_DESC_AT];
    uint32_t words[3] = { (uint32_t)sizeof AETHER_RV_NAME, (uint32_t)RV_DESC_SIZE, kind };
    memcpy(head, words, sizeof words);
    memset(head + sizeof words, 0, sizeof head - sizeof words);
    memcpy(head + sizeof words, AETHER_RV_NAME, sizeof AETHER_RV_NAME);
    if (!p || size < sizeof(AetherRvMark)) return NULL;
    for (size_t at = 0; at <= size - sizeof(AetherRvMark); at += 4)
        if (memcmp(p + at, head, sizeof head) == 0) return (const AetherRvMark*)(const void*)(p + at);
    return NULL;
}

/* The function a record names, as aether_rendezvous_find returns it. */
static void* rv_mark_fn(const AetherRvMark* m) {
#  if AETHER_RV_ELF
    int32_t off;
    memcpy(&off, (const unsigned char*)m + RV_DESC_AT, sizeof(off));
    return (void*)((uintptr_t)m + RV_DESC_AT + (uintptr_t)(intptr_t)off);
#  else
    aether_rv_fn f;
    memcpy(&f, (const unsigned char*)m + RV_DESC_AT, sizeof(f));
    void* p;
    memcpy(&p, &f, sizeof(p));
    return p;
#  endif
}
#endif

#if defined(_WIN32) && AETHER_RV_MARKS
/* The function the module's record of `kind` names (its .aerv section, from
 * the headers the loader maps with it), when it lies inside the module;
 * NULL otherwise. The module is held while it is read, and one another
 * thread has unloaded since the list was taken is skipped. */
static void* rv_pe_mark(HMODULE listed, uint32_t kind) {
    HMODULE m = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)(const void*)listed, &m) ||
        !m)
        return NULL;
    void* f = NULL;
    const unsigned char* base = (const unsigned char*)m;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)(const void*)base;
    if (m == listed && dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0) {
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(const void*)(base + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE) {
            const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
                if (memcmp(s[i].Name, AETHER_RV_SECTION "\0\0\0", IMAGE_SIZEOF_SHORT_NAME) != 0) continue;
                const AetherRvMark* mark =
                    rv_mark_in(base + s[i].VirtualAddress, (size_t)s[i].Misc.VirtualSize, kind);
                if (mark) {
                    uintptr_t a = (uintptr_t)rv_mark_fn(mark), lo = (uintptr_t)base;
                    if (a >= lo && a - lo < (uintptr_t)nt->OptionalHeader.SizeOfImage) f = rv_mark_fn(mark);
                }
                break;
            }
        }
    }
    FreeLibrary(m);
    return f;
}
#endif

#if defined(__APPLE__) && AETHER_RV_MARKS && defined(RTLD_NOLOAD)
/* The function image `i`'s record of `kind` names (its __DATA_CONST,
 * __aether_rv section), when it lies inside the image; NULL otherwise. An
 * image but the program is held while it is read (a dlopen of the path the
 * loader gives for its header, without loading), and read only while that
 * header is still an image's. */
static void* rv_macho_mark(uint32_t i, const struct mach_header* h, uint32_t kind) {
    void* held = NULL;
    if (i > 0) {
        Dl_info di;
        if (!dladdr(h, &di) || di.dli_fbase != (const void*)h || !di.dli_fname) return NULL;
        held = dlopen(di.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
        if (!held) return NULL;
        if (!dladdr(h, &di) || di.dli_fbase != (const void*)h) {
            dlclose(held);
            return NULL;
        }
    }
    void* f = NULL;
    unsigned long size = 0;
#  if defined(__LP64__)
    const uint8_t* sec = getsectiondata((const struct mach_header_64*)(const void*)h, AETHER_RV_SEGMENT,
                                        AETHER_RV_SECTNAME, &size);
#  else
    const uint8_t* sec = getsectiondata(h, AETHER_RV_SEGMENT, AETHER_RV_SECTNAME, &size);
#  endif
    const AetherRvMark* mark = sec ? rv_mark_in(sec, (size_t)size, kind) : NULL;
    if (mark) {
        void* q = rv_mark_fn(mark);
        Dl_info di;
        if (dladdr(q, &di) && di.dli_fbase == (const void*)h) f = q;
    }
    if (held) dlclose(held);
    return f;
}
#endif

#if defined(AETHER_RV_PHDR) && AETHER_RV_ELF
/* The function the first object with a record of `kind` names, read while
 * dl_iterate_phdr holds the loader's lock (glibc, musl, FreeBSD; NetBSD's
 * lets go around each call, so an object another thread unloads then may be
 * read as it goes): its notes, each inside the object's loaded segments, and
 * the function inside them too (#3442). The walk ends at the asking copy's
 * own object: none loaded after it is read, so none another thread is still
 * relocating. */
typedef struct { uint32_t kind; const void* self; void* fn; } RvWalk;

static int rv_collect(struct dl_phdr_info* info, size_t size, void* data) {
    (void)size;
    RvWalk* w = (RvWalk*)data;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (int k = 0; k < (int)info->dlpi_phnum; k++) {
        if (info->dlpi_phdr[k].p_type != PT_LOAD) continue;
        uintptr_t a = (uintptr_t)info->dlpi_addr + (uintptr_t)info->dlpi_phdr[k].p_vaddr;
        uintptr_t b = a + (uintptr_t)info->dlpi_phdr[k].p_memsz;
        if (a < lo) lo = a;
        if (b > hi) hi = b;
    }
    for (int k = 0; k < (int)info->dlpi_phnum; k++) {
        if (info->dlpi_phdr[k].p_type != PT_NOTE) continue;
        uintptr_t a = (uintptr_t)info->dlpi_addr + (uintptr_t)info->dlpi_phdr[k].p_vaddr;
        size_t n = (size_t)info->dlpi_phdr[k].p_memsz;
        if (a < lo || a >= hi || n > hi - a) continue;
        const AetherRvMark* m = rv_mark_in((const unsigned char*)a, n, w->kind);
        if (!m) continue;
        uintptr_t f = (uintptr_t)rv_mark_fn(m);
        if (f >= lo && f < hi) {
            w->fn = (void*)f;
            return 1;   /* the first: the walk ends */
        }
    }
    uintptr_t s = (uintptr_t)w->self;
    return (w->self && s >= lo && s < hi) ? 1 : 0;   /* the asking copy's own: no further */
}
#endif

void* aether_rendezvous_find(const char* symbol, uint32_t kind, const AetherRvMark* self) {
    void* p = NULL;
    (void)symbol;
    (void)kind;
    (void)self;
#if defined(_WIN32) && AETHER_RV_MARKS
    /* The modules in load order, the program first: the first with a
     * record, up to the asking copy's own. A snapshot of the list taken
     * under the loader's lock (a walk of the list without it fails or skips
     * while another thread loads or unloads one), retried while it says the
     * list changed under it, with no bound on the modules it holds. */
    HMODULE self_mod = NULL;
    if (self) GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 (LPCWSTR)(const void*)self, &self_mod);
    HANDLE snap = INVALID_HANDLE_VALUE;
    for (int tries = 0; tries < 1000 && snap == INVALID_HANDLE_VALUE; tries++) {
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
        if (snap == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH) break;
    }
    if (snap == INVALID_HANDLE_VALUE) return NULL;
    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    for (BOOL ok = Module32FirstW(snap, &me); ok && !p; ok = Module32NextW(snap, &me)) {
        p = rv_pe_mark(me.hModule, kind);
        if (self_mod && me.hModule == self_mod) break;   /* the asking copy's own: no further */
    }
    CloseHandle(snap);
#elif defined(__APPLE__) && AETHER_RV_MARKS && defined(RTLD_NOLOAD)
    /* The images in load order, the program first: the first with a
     * record, up to the asking copy's own. dyld's list is read by index
     * without its lock, so an image unloaded meanwhile shifts the rest: the
     * walk is made again while the count it read changes under it. */
    Dl_info sd;
    const void* self_hdr = (self && dladdr(self, &sd)) ? sd.dli_fbase : NULL;
    for (int tries = 0; tries < 8; tries++) {
        uint32_t n = _dyld_image_count();
        p = NULL;
        for (uint32_t i = 0; i < n && !p; i++) {
            const struct mach_header* h = _dyld_get_image_header(i);
            if (!h) continue;
            p = rv_macho_mark(i, h, kind);
            if (self_hdr && (const void*)h == self_hdr) break;   /* the asking copy's own */
        }
        if (_dyld_image_count() == n) break;
    }
#elif defined(AETHER_RV_PHDR) && AETHER_RV_ELF
    /* The loaded objects in load order, the program first: the first with a
     * record, up to the asking copy's own. */
    RvWalk w = { kind, self, NULL };
    dl_iterate_phdr(rv_collect, &w);
    p = w.fn;
#elif (defined(__unix__) || defined(__APPLE__)) && defined(RTLD_DEFAULT)
    /* The global scope in load order. */
    p = dlsym(RTLD_DEFAULT, symbol);
#endif
    return p;
}

/* Libc-compatible wrappers: the returned pointer has no hidden
 * header, so libc free() is safe (loses cap accounting but no
 * heap corruption). For full cap accounting, callers free with
 * aether_caps_free(p, bytes) passing the original size. */

/* What every allocation goes through when something is to be asked: the
 * test hook, the process cap, the account in use. */
static void* caps_alloc_slow(size_t bytes, int zeroed, size_t n, size_t size) {
    unsigned slow = atomic_load_explicit(&aether_caps_slow, memory_order_acquire);
    if ((slow & CAPS_SLOW_TEST) && aether_alloc_test_refuses()) {
        aether_alloc_failed(AETHER_STOP_OUT_OF_MEMORY);
        return NULL;
    }
    if (!aether_caps_check_alloc(bytes)) {
        aether_alloc_failed(AETHER_STOP_MEMORY_CAP);
        return NULL;
    }
    void* p = zeroed ? calloc(n, size) : malloc(bytes);
    if (!p) {
        aether_caps_account_free(bytes);
        aether_alloc_failed(AETHER_STOP_OUT_OF_MEMORY);
        return NULL;
    }
    if ((slow & CAPS_SLOW_ACCOUNTS) && !accounts()->alloc(p, bytes)) {
        free(p);
        aether_caps_account_free(bytes);
        aether_alloc_failed(AETHER_STOP_MEMORY_CAP);
        return NULL;
    }
    return p;
}

void* aether_caps_malloc(size_t bytes) {
    if (__builtin_expect(
            (atomic_load_explicit(&aether_caps_slow, memory_order_relaxed) & AETHER_CAPS_ALLOC_BITS) == 0, 1)) {
        void* p = malloc(bytes);
        if (!p) {
            aether_alloc_failed(AETHER_STOP_OUT_OF_MEMORY);
            return NULL;
        }
        atomic_fetch_add_explicit(&g_mem_used, (uint64_t)bytes, memory_order_relaxed);
        return p;
    }
    return caps_alloc_slow(bytes, 0, 0, 0);
}

void* aether_caps_calloc(size_t n, size_t size) {
    if (size != 0 && n > (size_t)-1 / size) {
        aether_alloc_failed(AETHER_STOP_OUT_OF_MEMORY);
        return NULL;
    }
    size_t bytes = n * size;
    if (__builtin_expect(
            (atomic_load_explicit(&aether_caps_slow, memory_order_relaxed) & AETHER_CAPS_ALLOC_BITS) == 0, 1)) {
        void* p = calloc(n, size);
        if (!p) {
            aether_alloc_failed(AETHER_STOP_OUT_OF_MEMORY);
            return NULL;
        }
        atomic_fetch_add_explicit(&g_mem_used, (uint64_t)bytes, memory_order_relaxed);
        return p;
    }
    return caps_alloc_slow(bytes, 1, n, size);
}

void* aether_caps_realloc(void* p, size_t old_bytes, size_t new_bytes) {
    unsigned slow = atomic_load_explicit(&aether_caps_slow, memory_order_acquire);
    if ((slow & CAPS_SLOW_TEST) && new_bytes > old_bytes && aether_alloc_test_refuses()) {
        aether_alloc_failed(AETHER_STOP_OUT_OF_MEMORY);
        return NULL;
    }
    if (new_bytes > old_bytes) {
        size_t delta = new_bytes - old_bytes;
        if (!aether_caps_check_alloc(delta)) {
            aether_alloc_failed(AETHER_STOP_MEMORY_CAP);
            return NULL;
        }
    }
    /* The accounts' view: the old block's bytes back to the account charged
     * for them, the new block's charged to the account in use, before the
     * realloc, so a refusal leaves the block as it was. */
    AetherExecAccount* old_owner = NULL;
    AetherExecAccount* now_in_use = NULL;
    int accounted = (slow & CAPS_SLOW_ACCOUNTS) != 0;
    if (accounted && !accounts()->realloc_begin(p, old_bytes, new_bytes, &old_owner, &now_in_use)) {
        if (new_bytes > old_bytes) aether_caps_account_free(new_bytes - old_bytes);
        aether_alloc_failed(AETHER_STOP_MEMORY_CAP);
        return NULL;
    }
    /* The old address, as a key only: the accounts read it again when the
     * realloc failed and the old block is still the one they charged. */
    uintptr_t old_key = (uintptr_t)p;
    void* np = realloc(p, new_bytes);
    if (accounted)
        accounts()->realloc_end((void*)old_key, new_bytes ? np : NULL, old_bytes, new_bytes, old_owner,
                                now_in_use);
    if (!np) {
        if (new_bytes > old_bytes) {
            aether_caps_account_free(new_bytes - old_bytes);
        }
        if (new_bytes) aether_alloc_failed(AETHER_STOP_OUT_OF_MEMORY);
        return NULL;
    }
    if (new_bytes < old_bytes) {
        aether_caps_account_free(old_bytes - new_bytes);
    }
    return np;
}

void aether_caps_free(void* p, size_t bytes) {
    if (!p) return;
    if (atomic_load_explicit(&aether_caps_slow, memory_order_relaxed) & CAPS_SLOW_ACCOUNTS)
        accounts()->free_block(p, bytes);
    free(p);
    aether_caps_account_free(bytes);
}

/* ------------------------------------------------------------------
 * Stop checks of an execution (#2956, runtime/aether_exec.h): its
 * deadline, or a cancel from another thread. Here, with the deadline they
 * read, so a C-backed std operation can ask without linking the execution
 * frames (the compiler links the std objects and this file).
 * ------------------------------------------------------------------ */


int aether_exec_stop_reason(void) {
    AetherFrames* F = aether_frames_here();
    if (aether_caps_deadline_tripped()) return AETHER_STOP_DEADLINE;
    AetherCancelCell* c = F->cell;
    if (!c) return 0;
    uint64_t s = atomic_load_explicit(&c->pending, memory_order_acquire);
    if (!s) return 0;
    /* The cancelled execution, or one nested inside it: the innermost goes
     * first; the cancelled one at its own next safe point, once the host
     * code between them has returned. One an actor step runs inside is not
     * this code's to stop, and is not spent either: it waits (#3392). */
    for (int i = F->exec_depth - 1; i >= 0; i--)
        if (F->exec[i].serial == s) return i >= F->exec_floor ? AETHER_STOP_CANCELLED : 0;
    /* A cancel for an execution that already ended: spent. One that has not
     * begun yet waits for it. */
    if (s <= F->serial)
        atomic_compare_exchange_strong_explicit(&c->pending, &s, 0, memory_order_acq_rel,
                                                memory_order_relaxed);
    return 0;
}

int aether_exec_should_stop(void) {
    if (!atomic_load_explicit(&aether_caps_armed, memory_order_relaxed)) return 0;
    AetherFrames* F = aether_frames_here();
    if (F->exec_depth <= F->exec_floor) return 0;
    /* Already stopping (an allocation failed, or an earlier check): stop. */
    if (F->exec[F->exec_depth - 1].stop) return 1;
    int why = aether_exec_stop_reason();
    if (why) F->exec[F->exec_depth - 1].stop = why;
    return why != 0;
}
