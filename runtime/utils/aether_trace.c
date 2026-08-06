/**
 * Message tracing implementation (#1333). See aether_trace.h for the design
 * and the cost argument.
 *
 * The whole file compiles to nothing without -DAETHER_TRACE, so a default
 * build carries no buffers and no code.
 */

#include "aether_trace.h"

#ifdef AETHER_TRACE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../scheduler/multicore_scheduler.h"

extern AETHER_TLS int current_core_id;
uint64_t aether_now_ns(void);

/* One extra slot past MAX_CORES for the main thread (core id -1). */
#define TRACE_SLOTS      (MAX_CORES + 1)
#define TRACE_MAIN_SLOT  (MAX_CORES)

/* Power of two so the wrap is a mask. 65536 events is 2 MiB per core, which is
 * a few seconds of a busy actor system; past that the ring keeps the most
 * recent events and counts what it dropped, so the output can say so rather
 * than silently presenting a truncated history as complete. */
#ifndef AETHER_TRACE_CAPACITY
#define AETHER_TRACE_CAPACITY 65536
#endif
#define TRACE_MASK (AETHER_TRACE_CAPACITY - 1)

typedef struct {
    AetherTraceEvent events[AETHER_TRACE_CAPACITY];
    uint64_t written;   /* total ever written; > capacity means it wrapped */
    /* Pad to keep one core's counter off another core's cache line. Two cores
     * never touch the same buffer, but adjacent `written` fields would still
     * share a line and ping-pong between them. */
    char pad[64];
} TraceBuffer;

static TraceBuffer g_trace_buffers[TRACE_SLOTS];

static const char* g_trace_path = NULL;
static int g_trace_state = -1;          /* -1 unresolved, 0 off, 1 on */
static int g_trace_flushed = 0;

/* Message-name table, registered by generated code. */
static const char* const* g_msg_names = NULL;
static int g_msg_name_count = 0;
static int g_msg_name_base  = 0;

static const char* const k_kind_names[AE_TRACE_KIND_COUNT] = {
    "send_local", "send_remote", "send_inline", "spsc_enqueue",
    "mailbox_send", "receive", "step_begin", "step_end", "drop_dead"
};

int aether_trace_enabled(void) {
    /* Resolved once. A traced build with AETHER_TRACE unset must still cost
     * nothing measurable, so this settles to a plain load of an int. */
    if (g_trace_state < 0) {
        const char* p = getenv("AETHER_TRACE");
        if (p && *p) {
            g_trace_path = p;
            g_trace_state = 1;
        } else {
            g_trace_state = 0;
        }
    }
    return g_trace_state;
}

void aether_trace_register_message_names(const char* const* names, int count, int base_id) {
    if (!names || count <= 0) return;
    g_msg_names = names;
    g_msg_name_count = count;
    g_msg_name_base = base_id;
}

void aether_trace_record(int kind, uint64_t actor_id, int msg_type, int sender_id) {
    int core = current_core_id;
    int slot = (core >= 0 && core < MAX_CORES) ? core : TRACE_MAIN_SLOT;

    TraceBuffer* b = &g_trace_buffers[slot];
    AetherTraceEvent* e = &b->events[b->written & TRACE_MASK];
    e->ts_ns     = aether_now_ns();
    e->actor_id  = actor_id;
    e->core      = core;
    e->kind      = kind;
    e->msg_type  = msg_type;
    e->sender_id = sender_id;
    b->written++;
}

static const char* trace_msg_name(int msg_type) {
    int idx = msg_type - g_msg_name_base;
    if (g_msg_names && idx >= 0 && idx < g_msg_name_count && g_msg_names[idx])
        return g_msg_names[idx];
    return NULL;
}

static void trace_write_event(FILE* f, const AetherTraceEvent* e) {
    const char* kind = (e->kind >= 0 && e->kind < AE_TRACE_KIND_COUNT)
                       ? k_kind_names[e->kind] : "unknown";
    const char* name = trace_msg_name(e->msg_type);

    fprintf(f, "{\"ts_ns\":%llu,\"core\":%d,\"event\":\"%s\",\"actor\":%llu,\"msg\":%d",
            (unsigned long long)e->ts_ns, e->core, kind,
            (unsigned long long)e->actor_id, e->msg_type);
    if (name) fprintf(f, ",\"msg_name\":\"%s\"", name);
    if (e->sender_id >= 0) fprintf(f, ",\"sender\":%d", e->sender_id);
    fputs("}\n", f);
}

void aether_trace_flush(void) {
    if (!aether_trace_enabled() || g_trace_flushed) return;
    g_trace_flushed = 1;

    FILE* f = fopen(g_trace_path, "w");
    if (!f) {
        fprintf(stderr, "aether: cannot open AETHER_TRACE file '%s'\n", g_trace_path);
        return;
    }

    /* Per-slot read cursors, then a repeated min-by-timestamp across slots.
     * Each buffer is already in timestamp order (a core writes in time order),
     * so merging them is a k-way merge and the whole file comes out ordered
     * without sorting anything. */
    uint64_t pos[TRACE_SLOTS];
    uint64_t end[TRACE_SLOTS];
    uint64_t dropped_total = 0;
    uint64_t total = 0;

    for (int s = 0; s < TRACE_SLOTS; s++) {
        uint64_t w = g_trace_buffers[s].written;
        if (w > AETHER_TRACE_CAPACITY) {
            dropped_total += w - AETHER_TRACE_CAPACITY;
            pos[s] = w - AETHER_TRACE_CAPACITY;   /* oldest surviving */
        } else {
            pos[s] = 0;
        }
        end[s] = w;
        total += end[s] - pos[s];
    }

    for (;;) {
        int best = -1;
        uint64_t best_ts = 0;
        for (int s = 0; s < TRACE_SLOTS; s++) {
            if (pos[s] >= end[s]) continue;
            uint64_t ts = g_trace_buffers[s].events[pos[s] & TRACE_MASK].ts_ns;
            if (best < 0 || ts < best_ts) { best = s; best_ts = ts; }
        }
        if (best < 0) break;
        trace_write_event(f, &g_trace_buffers[best].events[pos[best] & TRACE_MASK]);
        pos[best]++;
    }

    /* A trailing summary rather than a header, so the file is append-shaped and
     * a truncated run still yields readable events. `dropped` being non-zero is
     * the signal that the ring wrapped and the beginning of the run is gone. */
    fprintf(f, "{\"summary\":true,\"events\":%llu,\"dropped\":%llu,\"capacity_per_core\":%d}\n",
            (unsigned long long)total, (unsigned long long)dropped_total,
            AETHER_TRACE_CAPACITY);
    fclose(f);
}

#else  /* !AETHER_TRACE */

/* ISO C has no empty translation unit. Without the gate this file contributes
 * nothing, so give the compiler one declaration to chew on rather than rely on
 * every toolchain tolerating an empty file under -pedantic. */
typedef int aether_trace_disabled_translation_unit;

#endif /* AETHER_TRACE */
