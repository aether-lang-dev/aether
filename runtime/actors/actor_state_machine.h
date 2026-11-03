// Aether Actor Runtime - State Machine Implementation
// This header defines the lightweight message passing system

#ifndef AETHER_ACTOR_STATE_MACHINE_H
#define AETHER_ACTOR_STATE_MACHINE_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "../utils/aether_runtime_profile.h"
#include "../utils/aether_compiler.h"

#define ZEROCOPY_THRESHOLD 256  // Messages larger than this use zero-copy

// Message type enum (user-defined message types start at 100)
typedef enum {
    MSG_INCREMENT = 1,
    MSG_DECREMENT = 2,
    MSG_GET_VALUE = 3,
    MSG_SET_VALUE = 4,
    MSG_USER_START = 100
} MessageType;

// Generic message struct with zero-copy support for large payloads
typedef struct {
    int type;           // Message type
    int sender_id;      // ID of sending actor
    intptr_t payload_int;  // Integer/pointer payload (pointer-width for actor refs)
    void* payload_ptr;  // Pointer payload (small messages or zero-copy pointer)

    // Zero-copy support for large messages (payloads above ~256 bytes avoid the inline copy)
    struct {
        void* data;     // Owned data pointer
        int size;       // Data size
        int owned;      // The payload's kind: AETHER_MSG_ZC_BORROWED, _OWNED or AETHER_MSG_INLINE
    } zerocopy;
    void* _reply_slot;  // ActorReplySlot* for ask messages, NULL for fire-and-forget
} Message;

/* #3534: what zerocopy.owned says of a message's payload. BORROWED and
 * OWNED are the zero-copy buffer's (OWNED: zerocopy.data is the message's
 * to free). INLINE: a typed message's struct travels in the message itself,
 * its bytes from payload_int up to zerocopy.owned, so payload_ptr,
 * zerocopy.data and zerocopy.size are payload there and nothing may read
 * them as pointers or free them. Only a payload that fits is carried so
 * (aether_send_message.c), with no heap copy. */
#define AETHER_MSG_ZC_BORROWED 0
#define AETHER_MSG_ZC_OWNED    1
#define AETHER_MSG_INLINE      2

/* The bytes an inline payload has: 28 on 64-bit targets, 16 on 32-bit. */
#define AETHER_MSG_INLINE_BYTES (offsetof(Message, zerocopy.owned) - offsetof(Message, payload_int))

/* An inline payload copied out of its message, where every field of the
 * struct it holds is aligned (the bytes in the message are only aligned
 * as payload_int is). A struct that fits has no field aligned wider than
 * these members (one with a 16-byte field is 32 bytes or more), so the copy
 * is aligned for each of its fields, and an actor holding a copy keeps the
 * alignment it had. */
typedef union {
    long long ll;
    double d;
    void* p;
    unsigned char bytes[AETHER_MSG_INLINE_BYTES];
} AetherMsgInline;

static inline int aether_message_is_inline(const Message* msg) {
    return msg->zerocopy.owned == AETHER_MSG_INLINE;
}

/* The payload bytes of an inline message, in the message. */
static inline void* aether_message_inline_bytes(Message* msg) {
    return (void*)&msg->payload_int;
}

/* #3534: a typed message's payload, as C code that takes messages from a
 * mailbox itself reads it (a generated actor's step uses the actor's own
 * copy, aether_message_take_payload): an inline one in the message, which
 * holds a struct whose fields are no wider than a pointer at their natural
 * place; a heap one where payload_ptr points; NULL for none. */
static inline void* aether_message_payload(Message* msg) {
    return aether_message_is_inline(msg) ? aether_message_inline_bytes(msg) : msg->payload_ptr;
}

/* #3534: the payload's storage given back once its fields are released or
 * taken: a heap payload freed through the runtime that allocated it, an
 * inline one left with nothing to read. */
void aether_free_message(void* msg_data);
static inline void aether_message_free_payload(Message* msg) {
    if (aether_message_is_inline(msg)) {
        memset(aether_message_inline_bytes(msg), 0, AETHER_MSG_INLINE_BYTES);
        msg->zerocopy.owned = AETHER_MSG_ZC_BORROWED;
        return;
    }
    if (msg->payload_ptr) aether_free_message(msg->payload_ptr);
    msg->payload_ptr = NULL;
}

// Ring buffer mailbox for actors (power-of-2 for fast masking)
// 32 slots: large enough for typical burst patterns (skynet parents see ≤10
// child results; ping_pong sees 1; fork_join workers see 1) while keeping
// each actor's memory footprint small enough to scale to millions of actors.
// Overflow messages are re-queued via the scheduler's self-channel.
#define MAILBOX_SIZE 32   // Buffer for actor message queue
#define MAILBOX_MASK (MAILBOX_SIZE - 1)
typedef struct {
    Message messages[MAILBOX_SIZE];
    int head;
    int tail;
    // _Atomic int for the cross-thread SPSC handoff that work-stealing creates.
    // Normal operation: single scheduler thread owns producer AND consumer roles,
    // so all accesses are same-thread (relaxed would suffice).  But when work-
    // stealing transfers an actor from core A to core B mid-flight, core A may
    // still be writing a message (scheduler_send_local) while core B becomes the
    // new consumer.  The release on count++ in mailbox_send paired with the
    // acquire on count check in mailbox_receive establishes the necessary
    // happens-before: A's message-data write → A's count++ (release) →
    // B's count read (acquire) → B's message-data read.  Without this, B can
    // read stale or torn message data on ARM64 (weakly-ordered architectures).
    _Atomic int count;
#ifdef AETHER_DEBUG_MAILBOX
    atomic_flag _send_guard;
#endif
} Mailbox;

// Mailbox operations with optimization hints
// Note: Manual prefetch removed - benchmarks showed 16% performance loss
// Hardware prefetcher handles sequential ring buffer access more efficiently
static inline int AETHER_HOT mailbox_send(Mailbox* AETHER_RESTRICT mbox, Message msg) {
    PROFILE_START();
    // Relaxed load: producer does not need to observe consumer's count decrements
    // with strict ordering — a stale "full" view just causes a re-queue (correct).
    if (unlikely(atomic_load_explicit(&mbox->count, memory_order_relaxed) >= MAILBOX_SIZE)) return 0;

#ifdef AETHER_DEBUG_MAILBOX
    // SPSC assertion: detect concurrent producers
    if (unlikely(atomic_flag_test_and_set_explicit(&mbox->_send_guard, memory_order_acquire))) {
        fprintf(stderr, "CONCURRENT mailbox_send! count=%d tail=%d msg_type=%d\n",
                atomic_load_explicit(&mbox->count, memory_order_relaxed), mbox->tail, msg.type);
    }
#endif
    mbox->messages[mbox->tail] = msg;
    mbox->tail = (mbox->tail + 1) & MAILBOX_MASK;  // Fast power-of-2 masking
    // Release: publishes message-data write (and any prior write, e.g. actor->active=1)
    // to any thread that subsequently acquires on count (mailbox_receive).
    atomic_fetch_add_explicit(&mbox->count, 1, memory_order_release);
#ifdef AETHER_DEBUG_MAILBOX
    atomic_flag_clear_explicit(&mbox->_send_guard, memory_order_release);
#endif
    PROFILE_END_MAILBOX_SEND(0);  // Core ID determined at runtime in real usage
    return 1;
}

static inline int AETHER_HOT mailbox_receive(Mailbox* AETHER_RESTRICT mbox, Message* AETHER_RESTRICT out_msg) {
    PROFILE_START();
    // Acquire: synchronizes-with the release in mailbox_send — ensures the message
    // data written by the producer is visible before we read it below.
    if (unlikely(atomic_load_explicit(&mbox->count, memory_order_acquire) == 0)) return 0;

    *out_msg = mbox->messages[mbox->head];
    mbox->head = (mbox->head + 1) & MAILBOX_MASK;  // Fast power-of-2 masking
    // Relaxed: only the consumer decrements count; no other thread observes it with
    // acquire, so no ordering guarantee is needed here.
    atomic_fetch_sub_explicit(&mbox->count, 1, memory_order_relaxed);
    PROFILE_END_MAILBOX_RECEIVE(0);
    return 1;
}

static inline void mailbox_init(Mailbox* mbox) {
    mbox->head = 0;
    mbox->tail = 0;
    atomic_store_explicit(&mbox->count, 0, memory_order_relaxed);
#ifdef AETHER_DEBUG_MAILBOX
    atomic_flag_clear(&mbox->_send_guard);
#endif
}

// Batch operations for high-throughput scenarios.
// Both batch functions are always called by the actor's owning scheduler thread
// (single-producer or single-consumer context), so relaxed atomics suffice.
static inline int AETHER_HOT mailbox_receive_batch(
    Mailbox* AETHER_RESTRICT mbox,
    Message* AETHER_RESTRICT out_msgs,
    int max_count)
{
    int received = 0;
    // Acquire on the first count check so any release-paired sends are visible.
    while (received < max_count &&
           atomic_load_explicit(&mbox->count, received == 0 ? memory_order_acquire
                                                            : memory_order_relaxed) > 0) {
        out_msgs[received] = mbox->messages[mbox->head];
        mbox->head = (mbox->head + 1) & MAILBOX_MASK;  // Fast power-of-2 masking
        atomic_fetch_sub_explicit(&mbox->count, 1, memory_order_relaxed);
        received++;
    }
    return received;
}

static inline int AETHER_HOT mailbox_send_batch(
    Mailbox* AETHER_RESTRICT mbox,
    const Message* AETHER_RESTRICT msgs,
    int count)
{
    int sent = 0;
    while (sent < count &&
           atomic_load_explicit(&mbox->count, memory_order_relaxed) < MAILBOX_SIZE) {
        mbox->messages[mbox->tail] = msgs[sent];
        mbox->tail = (mbox->tail + 1) & MAILBOX_MASK;  // Fast power-of-2 masking
        atomic_fetch_add_explicit(&mbox->count, 1, memory_order_release);
        sent++;
    }
    return sent;
}

// Zero-copy message helpers
static inline Message message_create_zerocopy(int type, int sender_id, void* data, int size) {
    Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = type;
    msg.sender_id = sender_id;
    msg.zerocopy.data = data;
    msg.zerocopy.size = size;
    msg.zerocopy.owned = AETHER_MSG_ZC_OWNED;
    return msg;
}

static inline Message message_create_simple(int type, int sender_id, intptr_t payload) {
    Message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = type;
    msg.sender_id = sender_id;
    msg.payload_int = payload;
    return msg;
}

static inline void message_free(Message* msg) {
    if (msg->zerocopy.owned == AETHER_MSG_ZC_OWNED && msg->zerocopy.data) {
        free(msg->zerocopy.data);
        msg->zerocopy.data = NULL;
        msg->zerocopy.size = 0;
        msg->zerocopy.owned = AETHER_MSG_ZC_BORROWED;
    }
}

static inline void message_transfer(Message* dest, Message* src) {
    *dest = *src;
    if (src->zerocopy.owned == AETHER_MSG_ZC_OWNED) {
        src->zerocopy.owned = AETHER_MSG_ZC_BORROWED;  // Transfer ownership
        src->zerocopy.data = NULL;
    } else if (src->zerocopy.owned == AETHER_MSG_INLINE) {
        /* #3534: the payload's bytes moved with the copy; the source keeps
         * none of them, so none is read as a pointer there. */
        aether_message_free_payload(src);
    }
}

#endif // AETHER_ACTOR_STATE_MACHINE_H
