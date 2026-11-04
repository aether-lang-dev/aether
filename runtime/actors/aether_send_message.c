#include "actor_state_machine.h"
#include "../utils/aether_fatal.h"   /* aether_fatal_flush (#3435) */
#include "../scheduler/multicore_scheduler.h"
#include "../config/aether_optimization_config.h"
#include "../utils/aether_compiler.h"
#include <string.h>
#include <stdlib.h>
#include "../utils/aether_thread.h"
#include "../../std/string/aether_string.h"
#include "../aether_resource_caps.h"
#include "aether_frames.h"
#include "aether_panic.h"

// Thread-local core ID for optimization
extern AETHER_TLS int current_core_id;

// ActorBase is defined in multicore_scheduler.h

// ==============================================================================
// Message payloads
// ==============================================================================
// A typed message's payload travels in the message when it fits its payload
// area (AETHER_MSG_INLINE, #3534), else as a heap copy (payload_message
// below), freed by whoever handles or drops the message. There is no
// payload pool (#3374):
// its acquire side was removed because an actor can move to another core
// after a message is sent, so a pooled payload would be released by a
// thread that does not own the pool.

/* Deprecated (#3374): there is no pool, so there is nothing to count; kept
 * for callers built against it, it reports zeros. */
void aether_message_pool_stats(uint64_t* hits, uint64_t* misses, uint64_t* large) {
    if (hits) *hits = 0;
    if (misses) *misses = 0;
    if (large) *large = 0;
}

void aether_free_message(void* msg_data) {
    free(msg_data);
}

void aether_message_drop(ActorBase* target, Message* msg) {
    if (aether_message_is_inline(msg)) {
        /* #3534: the payload is the message's own bytes. Its fields are
         * released from an aligned copy, and the message holds nothing
         * from before the release runs, so a release that panics leaves
         * nothing behind to free twice. */
        AetherMsgInline payload;
        memcpy(payload.bytes, aether_message_inline_bytes(msg), AETHER_MSG_INLINE_BYTES);
        aether_message_free_payload(msg);
        if (msg->type == AETHER_MSG_DOWN) {
            /* A Down a program sent fits too: its reason is released as
             * a heap Down's is, below. */
            string_release(((AetherDownMsg*)payload.bytes)->reason);
        } else if (target && target->release_message) {
            target->release_message(msg->type, payload.bytes);
        }
        return;
    }
    if (msg->payload_ptr) {
        if (msg->type == AETHER_MSG_DOWN) {
            // The runtime wrote it (#2974): its reason is the runtime's to
            // release, whether or not the program declares Down.
            string_release(((AetherDownMsg*)msg->payload_ptr)->reason);
        } else if (target && target->release_message) {
            target->release_message(msg->type, msg->payload_ptr);
        }
        aether_free_message(msg->payload_ptr);
        msg->payload_ptr = NULL;
    }
    if (msg->zerocopy.owned == AETHER_MSG_ZC_OWNED && msg->zerocopy.data) {
        free(msg->zerocopy.data);
        msg->zerocopy.data = NULL;
        msg->zerocopy.owned = AETHER_MSG_ZC_BORROWED;
    }
}

/* #3401: the drop of a message an ending actor leaves undelivered. Its
 * release is the program's code for a field of an owned type (a drop
 * function, #3142), which may panic, and the finalize that drains the
 * actor ran it bare: a panic there jumped out of the finalize (to the
 * nearest frame below, or ended the program without one), so the messages
 * after it were not released and the actor's watchers and links were never
 * told. The release runs in a containment frame of its own instead
 * (aether_contain): a panic in it is caught there, the unwind journal
 * giving back what the release held at its depth, and reported as the step
 * barrier reports a panic in the release that follows a handler, through
 * the death hook (itself contained, #3417) with
 * the actor's id and the panic's reason, which is then released. The
 * actor's end has its reason already and keeps it. What the release did not
 * get to free (the payload, an owned zero-copy buffer) goes here, and the
 * finalize goes on with the next message. A message whose release is not
 * the program's (no release_message, a runtime Down) needs no frame. The
 * step barrier drops the message a panicking handler ran on through here
 * too (#3403). */
typedef struct { ActorBase* target; Message* msg; } DropCall;

static void drop_body(void* p) {
    DropCall* c = (DropCall*)p;
    aether_message_drop(c->target, c->msg);
}

/* The drop of `msg` in a containment frame (#3403): 0 when it was dropped,
 * 1 when its release panicked: what the release did not get to free (the
 * payload, an owned zero-copy buffer) is freed here, and *reason and
 * *reason_release are the panic's, the caller's to report and release. A
 * drop cut by an interruption of the call it runs inside (#3404) leaves
 * the same freed and reports nothing: the call ends with it. */
int aether_message_drop_contained(ActorBase* target, Message* msg, const char** reason,
                                  void (**reason_release)(const void*)) {
    int has_payload = aether_message_is_inline(msg) || msg->payload_ptr;
    if (!target || !target->release_message || !has_payload || msg->type == AETHER_MSG_DOWN) {
        aether_message_drop(target, msg);
        return 0;
    }
    DropCall c = { target, msg };
    int how = aether_contain(drop_body, &c, reason, reason_release);
    if (how == 0) return 0;
    /* An inline payload was emptied before its release ran (#3534). */
    if (!aether_message_is_inline(msg) && msg->payload_ptr) {
        aether_free_message(msg->payload_ptr);
        msg->payload_ptr = NULL;
    }
    if (msg->zerocopy.owned == AETHER_MSG_ZC_OWNED && msg->zerocopy.data) {
        free(msg->zerocopy.data);
        msg->zerocopy.data = NULL;
        msg->zerocopy.owned = AETHER_MSG_ZC_BORROWED;
    }
    return how == 1;
}

void aether_message_drop_at_end(ActorBase* target, Message* msg) {
    const char* reason = NULL;
    void (*reason_release)(const void*) = NULL;
    if (!aether_message_drop_contained(target, msg, &reason, &reason_release)) return;
    /* In a frame of its own, as the step barrier's hook is (#3417): a hook
     * that panics is reported on stderr and the finalize goes on. Fired
     * bare, its panic left the finalize halfway (the messages it had drained
     * never credited, so scheduler_shutdown waited for them forever, and the
     * watchers never told), and the finalize, marked done, never ran again.
     * Nothing a finalize runs leaves it now; a link's drain still journals
     * its queue for a panic that would (#3400). */
    aether_fire_death_hook_contained(target->id, reason);
    if (reason_release) reason_release(reason);
}

// The heap copy of a typed message's payload, or NULL when there was no
// memory for it.
// A payload that does not fit the message is always a heap allocation.
// TLS pools have thread affinity issues: a same-core actor may be migrated
// to another core after the message is sent, causing the receiver to call
// free() on pool memory (heap-use-after-free / free-on-non-malloc).
static void* payload_copy(void* message_data, size_t message_size) {
    void* msg_copy = aether_alloc_test_refuses() ? NULL : malloc(message_size);
    if (msg_copy) memcpy(msg_copy, message_data, message_size);
    return msg_copy;
}

static Message message_of(void* payload, int type, void* reply_slot) {
    Message msg;
    msg.type = type;
    msg.sender_id = 0;
    msg.payload_int = 0;
    msg.payload_ptr = payload;
    msg.zerocopy.data = NULL;
    msg.zerocopy.size = 0;
    msg.zerocopy.owned = AETHER_MSG_ZC_BORROWED;
    msg._reply_slot = reply_slot;
    return msg;
}

/* #3534: a typed message whose struct fits the message's payload area
 * (AETHER_MSG_INLINE_BYTES) travels in it: its bytes are copied into the
 * message, which carries them by value through every queue, with no heap
 * copy to make or free. What the fields own moves with the bytes, as it
 * does into a heap copy. */
static int payload_fits_inline(size_t message_size) {
    return message_size <= AETHER_MSG_INLINE_BYTES;
}

static Message inline_message(void* message_data, size_t message_size, void* reply_slot) {
    Message msg = message_of(NULL, *(int*)message_data, reply_slot);
    memcpy(aether_message_inline_bytes(&msg), message_data, message_size);
    msg.zerocopy.owned = AETHER_MSG_INLINE;
    return msg;
}

// The heap copy of a typed message's payload, as the Message a send carries,
// or the payload in the message when it fits (#3534).
static Message payload_message(ActorBase* actor, void* message_data, size_t message_size) {
    if (payload_fits_inline(message_size)) return inline_message(message_data, message_size, NULL);
    void* msg_copy = payload_copy(message_data, message_size);
    if (!msg_copy) {
        /* Inside a library call (#2956): nothing is queued yet, so the
         * message's own values are released (they moved into it) and the
         * call ends with AETHER_OUT_OF_MEMORY. Elsewhere the process stops,
         * as it always did: the send has no way to fail. */
        AetherFrames* F = aether_frames_here();
        if (F->exec_depth > F->exec_floor) {
            if (actor->release_message) actor->release_message(*(int*)message_data, message_data);
            aether_out_of_memory("out of memory copying a message");
        }
        aether_fatal_flush();   /* the program's lines first (#3435, aether_fatal.h) */
        fprintf(stderr, "aether: malloc(%zu) failed for msg type %d to actor %d\n",
                message_size, *(int*)message_data, actor->id);
        abort();
    }
    return message_of(msg_copy, *(int*)message_data, NULL);
}

// An ask's message (#3387): the payload copied (into the message when it
// fits, #3534), the asker's reply slot in it.
// An ask can fail, so a copy that finds no memory is its failure, not an
// unwind: 0, with what the message owns released (its values moved into it)
// and nothing sent. The slot is passed here rather than through a
// thread-local the send reads: the send can step its target inline, and
// every payload send that handler made read the asker's slot too.
int aether_ask_message(void* actor_ptr, void* message_data, size_t message_size,
                       void* reply_slot, Message* out) {
    ActorBase* actor = (ActorBase*)actor_ptr;
    if (payload_fits_inline(message_size)) {   /* #3534 */
        *out = inline_message(message_data, message_size, reply_slot);
        return 1;
    }
    void* msg_copy = payload_copy(message_data, message_size);
    if (!msg_copy) {
        if (actor && actor->release_message) actor->release_message(*(int*)message_data, message_data);
        return 0;
    }
    *out = message_of(msg_copy, *(int*)message_data, reply_slot);
    return 1;
}

// Send a typed message to an actor. The payload is copied (into the message
// when it fits, #3534, else to the heap) and the message handed to the
// scheduler, which delivers it whatever the actor count
// (#3112): the handler runs later, on a scheduler thread, and the sender
// goes on. The cooperative runtime queues it for the next wait_for_idle or
// aether_scheduler_poll.
void aether_send_message(void* actor_ptr, void* message_data, size_t message_size) {
    ActorBase* actor = (ActorBase*)actor_ptr;
    aether_send_built(actor, payload_message(actor, message_data, message_size));
}

// A message built already (an ask's), to its actor: through this core when
// the actor is this core's, else through its own.
void aether_send_built(ActorBase* actor, Message msg) {
#if AETHER_HAS_THREADS
    // Cache TLS once — avoids repeated tlv_get_addr calls on macOS
    const int my_core = current_core_id;

    if (my_core >= 0 && my_core == atomic_load_explicit(&actor->assigned_core, memory_order_relaxed)) {
        scheduler_send_local(actor, msg);
    } else {
        scheduler_send_remote(actor, msg, my_core);
    }
#else
    scheduler_send_local(actor, msg);
#endif
}

// try_send(ref, Msg {...}) of a payload message (#2974): sent as
// aether_send_message sends it, unless the actor is dead or its mailbox
// full, which it says ("" when sent).
const char* aether_try_send_message(void* actor_ptr, void* message_data, size_t message_size) {
    ActorBase* actor = (ActorBase*)actor_ptr;
    if (!actor) return "the actor is dead";
    return scheduler_try_send(actor, payload_message(actor, message_data, message_size));
}

// A typed message sent inside a batched loop (#3117): it joins the batch, as
// the loop's one-int messages do, so the loop's sends keep their order.
void aether_send_message_batch(void* actor_ptr, void* message_data, size_t message_size) {
    ActorBase* actor = (ActorBase*)actor_ptr;
    scheduler_send_batch_add(actor, payload_message(actor, message_data, message_size));
}
