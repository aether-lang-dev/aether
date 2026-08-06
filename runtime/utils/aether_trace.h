/**
 * Message tracing (#1333).
 *
 * Records the actual delivery path of every actor message so a run can be
 * replayed after the fact: which send path was taken, which queue the message
 * went through, when it was received, and which step processed it.
 *
 * COST WHEN OFF
 * -------------
 * Compiled out entirely. Without -DAETHER_TRACE every macro below is
 * `((void)0)` and no field, buffer or branch survives into the binary. This is
 * deliberate rather than a runtime flag: message send is the runtime's core
 * loop, and a predictable branch is still a branch plus an inhibited
 * optimisation on a path that runs millions of times a second. The previous
 * tracing sketch (removed in PR #1330) had no activation story at all and was
 * dead code in every build; the flag is what keeps this one honest.
 *
 * `ae build --trace prog.ae` compiles the runtime from source with the flag,
 * so nobody has to hand-roll a runtime rebuild to use it.
 *
 * COST WHEN ON
 * ------------
 * One timestamp and one 32-byte store into a per-core ring buffer. No locks
 * and no atomics on the hot path: a core only ever writes its OWN buffer, and
 * the buffers are read after scheduler_shutdown() has joined every thread, so
 * the join provides the happens-before the merge needs.
 *
 * OUTPUT
 * ------
 * `AETHER_TRACE=<path>` selects the output file; unset means the buffers stay
 * empty and nothing is written even in a traced build. Events are merged by
 * timestamp across cores and written as JSONL, one event per line, because a
 * trace is read with grep and jq far more often than by a viewer.
 */

#ifndef AETHER_TRACE_H
#define AETHER_TRACE_H

#include <stdint.h>

/* Event kinds. The set exists to answer "which path did this message actually
 * take", so the queue choices are distinct events rather than a flag: a
 * message that went through the SPSC queue and one that went through the
 * mailbox are the two cases whose difference a trace is usually opened to
 * explain. */
typedef enum {
    AE_TRACE_SEND_LOCAL = 0,   /* scheduler_send_local entered              */
    AE_TRACE_SEND_REMOTE,      /* scheduler_send_remote entered             */
    AE_TRACE_SEND_INLINE,      /* the direct-send bypass ran step() inline  */
    AE_TRACE_SPSC_ENQUEUE,     /* delivered to an auto_process actor's SPSC */
    AE_TRACE_MAILBOX_SEND,     /* delivered to the actor's mailbox          */
    AE_TRACE_RECEIVE,          /* dequeued for processing                   */
    AE_TRACE_STEP_BEGIN,       /* step() entered for this message           */
    AE_TRACE_STEP_END,         /* step() returned                           */
    AE_TRACE_DROP_DEAD,        /* dropped: recipient already dead           */
    AE_TRACE_KIND_COUNT
} AetherTraceKind;

#ifdef AETHER_TRACE

#include <stddef.h>

typedef struct {
    uint64_t ts_ns;
    uint64_t actor_id;
    int32_t  core;
    int32_t  kind;
    int32_t  msg_type;
    int32_t  sender_id;
} AetherTraceEvent;

/* Record one event on the calling core. `core` < 0 (the main thread) lands in
 * its own buffer rather than being folded into core 0, so a trace shows
 * main-thread sends as what they are. */
void aether_trace_record(int kind, uint64_t actor_id, int msg_type, int sender_id);

/* Is tracing armed? Reads a value fixed at first use, so the callers below can
 * skip the timestamp when AETHER_TRACE is unset in the environment. */
int aether_trace_enabled(void);

/* Merge every core's buffer by timestamp and write the JSONL file. Called from
 * scheduler_shutdown() AFTER the threads are joined; safe to call more than
 * once, writes once. */
void aether_trace_flush(void);

/* Message-id to name table, emitted by codegen from the message registry so a
 * trace shows `Ping` rather than `102`. Ids below the base are runtime
 * internals and are named from a built-in table. Registering is idempotent and
 * the pointers are borrowed: codegen emits them as static storage. */
void aether_trace_register_message_names(const char* const* names, int count, int base_id);

#define AETHER_TRACE_EVENT(kind, actor_id, msg_type, sender_id)                 \
    do {                                                                        \
        if (aether_trace_enabled())                                             \
            aether_trace_record((kind), (uint64_t)(actor_id),                   \
                                (int)(msg_type), (int)(sender_id));             \
    } while (0)

#define AETHER_TRACE_FLUSH() aether_trace_flush()

#else  /* !AETHER_TRACE */

#define AETHER_TRACE_EVENT(kind, actor_id, msg_type, sender_id) ((void)0)
#define AETHER_TRACE_FLUSH()                                    ((void)0)

#endif /* AETHER_TRACE */

#endif /* AETHER_TRACE_H */
