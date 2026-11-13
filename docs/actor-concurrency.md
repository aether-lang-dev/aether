# Aether Runtime Guide

Understanding the actor runtime, concurrency model, and message passing system.

## Performance Optimizations

The actor runtime includes several performance optimizations applied automatically. See [runtime-optimizations.md](runtime-optimizations.md) for detailed descriptions of each technique.

### Scheduler Design

**Partitioned Multicore Scheduler:**
- Locality-aware actor placement at spawn time (actors placed on the caller's core)
- Each core processes only its local actors in the fast path
- Work stealing activated when cores become idle
- Non-blocking work stealing using try-lock with ascending core-id lock ordering
- NUMA-aware allocation when available (Linux and Windows; on macOS `aether_numa_alloc` is a plain `malloc`)
- Message-driven actor migration co-locates communicating actors on the sender's core: an actor a message from another core reaches moves to that core before it runs it, so a chain of sends (a ring, a pipeline) gathers on one core in its first round, where its sends are local
- An actor moves only by its own core, and only while no message for it is on its way through a core's queues (each such message is counted in the actor's `inflight`), so the messages one actor sends another arrive in order across a move (#3090, #3093)

**Work Stealing:**
- Triggered after extended idle scheduler cycles
- Selects the core with the most pending work
- Takes entire actors (not individual messages) to preserve cache locality: the idle core asks the busy one for an actor, and the busy core moves it, as every move is made
- Uses ascending core-id lock ordering consistent with migration to prevent deadlock

**Message Delivery:**
- Same-core messages: direct mailbox write (no queue overhead)
- Cross-core messages: lock-free queue with batch dequeue
- Adaptive batch processing: 64 to 1024 messages per scheduler cycle

### Active Optimizations

**Sends are asynchronous, whatever the actor count:** the sender goes on and
a scheduler thread runs the handler, from the first actor on (#3112).

**Message Coalescing:**
- Batch dequeue drains multiple messages in a single atomic operation
- Configurable threshold (SCHEDULER_DRAIN_BATCH = 512)

**Direct Send:**
- Bypasses incoming queue for same-core actors
- Writes directly to the actor's mailbox
- Guarded by `current_core_id` to prevent data races from non-scheduler threads

**Adaptive Batching:**
- Dynamically adjusts batch size based on queue depth
- Increases under sustained load, decreases when idle

**Inline Single-Int Messages:**
- Messages with one integer field need no payload allocation at all
- Value encoded directly in `Message.payload_int`

## Actor Model

Aether implements a lightweight actor model where actors are state machines that communicate via asynchronous messages.

### Actor Lifecycle

1. **Spawn** - Create actor instance with `spawn(ActorName())`
2. **Send** - Send messages with the `!` operator or generated send functions
3. **Step** - Scheduler calls the actor's step function to process mailbox messages
4. **Deactivate** - Actor is marked inactive when its mailbox is empty
5. **End** - A panic in its handler, `actors.kill(ref)`, `actors.stop()` from its own handler, or a C host's release ends it, once ([The end of an actor](#the-end-of-an-actor))

### Actor Structure

Each actor is a C struct whose first fields are `ActorBase`'s, since the
scheduler casts a pointer to it to `ActorBase*`. The fields are declared once,
in the `AETHER_ACTOR_BASE_FIELDS` macro (`runtime/scheduler/multicore_scheduler.h`),
and an actor struct expands it first, so its layout cannot drift from the base:

```c
typedef struct {
    AETHER_ACTOR_BASE_FIELDS
    int count;                       // the actor's own state follows
} CounterActor;
```

The fields, in order: `active`, `id`, `mailbox`, `step`, `step_payload`, `reply_slot`, `timer_slot`, `step_type`, `step_lock`, `step_inline`, `exit_flags`, `ready_queued`, `scheduler_owned`, `dead`, `mailbox_bound`, `assigned_core`, `auto_process`, `inflight`, `migrate_to`, `mailbox_pending`, `full_drops`, `thread_parked`, `incarnation`, `inbox`, `death_watch`, `thread`, `timeout_ns`, `last_activity_ns`, `alloc_size`, `destroy_state`, `release_message`, `spsc_queue`. The header comments each one.

An actor the compiler generates holds the base as its first member instead, `ActorBase _aether_base;`, followed by its states. So a state can be named like any of these fields (`state id`, `state active`); only a state named `_aether_base` is refused (#3094).

## Message Passing

### Mailbox

Each actor has a ring buffer mailbox with 32 slots (power-of-2 for fast masking):

```c
#define MAILBOX_SIZE 32
#define MAILBOX_MASK (MAILBOX_SIZE - 1)

typedef struct {
    Message messages[MAILBOX_SIZE];
    int head;
    int tail;
    _Atomic int count;
} Mailbox;
```

The mailbox is a single-producer single-consumer (SPSC) queue. The owning scheduler thread delivers messages and calls the actor's step function. `count` is `_Atomic` to handle the rare case where work-stealing transfers an actor between cores mid-flight. Cross-core messages arrive via the scheduler's per-sender lock-free incoming queues and are delivered to the mailbox by the owning core.

### A bounded mailbox

The 32 slots are not a limit: what a full mailbox cannot take waits in the core queues, the senders' deferred sends and the actor's overflow, without bound. An actor declared `@mailbox(bound: N)` (#2974) takes at most N messages that its step has not taken yet, wherever they wait. The generated spawn sets `ActorBase.mailbox_bound`; `mailbox_pending` counts the messages accepted for the actor and not yet taken or dropped:

- A send takes its place before it routes (`aether_mailbox_admit`, a compare-and-swap loop, exact: refused only while N are pending), in `scheduler_send_local` and `scheduler_send_remote`; `scheduler_send_many` sends a bounded actor's messages through them one at a time, and the main thread's batch takes each one's place as it joins the batch. The bound sits beside `dead`, on the line of the fields a send from another core reads (#3540), so an unbounded actor's send pays one load and a branch on a line it reads anyway.
- Refused, it is never blocked on (a blocked sender can be a core's thread that the actor's own core waits on): a plain `!` is released and counted (`ActorBase.full_drops`, `scheduler_full_drops()`, `actors.full_drops(ref)`), reported once on stderr; an ask fails at once with `AETHER_ASK_FULL`, "the mailbox is full"; `scheduler_try_send` (`try_send(ref, Msg { ... })`) returns the reason, "the mailbox is full" or "the actor is dead", to its caller, and counts nothing.
- A place comes back when the generated step takes the message (`aether_mailbox_took`, emitted only for a bounded actor) or when a drop releases it before that (`release_undelivered` and the cooperative scheduler's drops: a death's drain, a message on its way to a dead actor, a teardown).
- System messages (`Down`, `IoReady`) are neither counted nor refused.

### Message Structure

```c
typedef struct {
    int type;           // Message type ID
    int sender_id;      // Sender actor ID
    intptr_t payload_int; // Integer payload (intptr_t to avoid truncation of actor refs on 64-bit)
    void* payload_ptr;  // A typed message's heap copy (unless it travels inline)
    struct {
        void* data;
        int size;
        int owned;      // The payload's kind: AETHER_MSG_ZC_BORROWED, _OWNED, AETHER_MSG_INLINE
    } zerocopy;
    void* _reply_slot;  // ActorReplySlot* for ask messages, NULL for fire-and-forget
} Message;
```

### Sending Messages

Messages are sent asynchronously. The generated code routes messages through the scheduler:

```c
// Same-core: direct delivery
if (current_core_id >= 0 && current_core_id == atomic_load_explicit(&actor->assigned_core, memory_order_relaxed)) {
    scheduler_send_local(actor, msg);
} else {
    scheduler_send_remote(actor, msg, current_core_id);
}
```

For single-int messages, the code generator emits an inline path that stores the value directly in `msg.payload_int`, with no payload allocation.

Any other typed message whose struct fits the bytes from `payload_int` up to `zerocopy.owned` (28 on 64-bit targets, 16 on 32-bit: two ints, an int and a string, a `Down`) travels in the message itself (#3534): the send copies the struct there and sets `zerocopy.owned` to `AETHER_MSG_INLINE`, so there is no heap copy to make or free, and a send under memory pressure cannot fail for want of one. What its fields own moves with the bytes, as it moves into a heap copy. Only a larger struct is copied to the heap. C code that takes messages from a mailbox itself reads either with `aether_message_payload(&msg)` and gives either back with `aether_message_free_payload(&msg)` ([C embedding](c-embedding.md#message-handler-pattern)).

A `ptr` field (a `std.bytes` handle, a `*T`) crosses as the pointer, with no copy. To hand the buffer over rather than share it, send it with `move`: `r ! Chunk { data: move buf }`. The sender's `buf` is then moved-from, and any later use of it is a compile error naming the line of the move. See [`move`](isolated.md#move-handing-a-buffer-over-2975).

### Receiving Messages

The `receive` block in the actor definition becomes the step function. The code generator emits a computed goto dispatch table for message handler selection:

```c
void _aether_step_ActorName(ActorName* self) {
    Message msg;
    if (!mailbox_receive(&self->base.mailbox, &msg)) {
        self->base.active = 0;
        return;
    }
    // A payload that travelled in the message is copied to the actor's
    // _aether_step_inline, the last member of its struct (#3534).
    void* _msg_data = aether_message_take_payload(&self->_aether_base, &msg,
                                                  &self->_aether_step_inline);
    int _msg_id = msg.type;

    static void* dispatch_table[256] = {
        [1] = &&handle_MessageA,
        [2] = &&handle_MessageB,
    };

    if (_msg_id >= 0 && _msg_id < 256 && dispatch_table[_msg_id]) {
        goto *dispatch_table[_msg_id];
    }
    aether_message_drop(&self->_aether_base, &msg);   // no handler for it
    return;

    handle_MessageA:
        self->_aether_base.step_payload = _msg_data;
        self->_aether_base.step_type = _msg_id;
        ActorName_handle_MessageA(self, _msg_data);
        self->_aether_base.step_payload = NULL;
        _aether_msg_release_MessageA(_msg_data);   // what its fields own
        aether_message_payload_done(_msg_data, &self->_aether_step_inline);  // a heap copy is freed
        return;
}
```

### A message that is not handled

A handled message's fields are released by the step after its handler: `_aether_msg_release_<Msg>` drops what they own (a string the send copied, a closure, a struct that owns), then the payload is freed. A message that is dropped instead is released the same way (#3142). Each program emits one `_aether_release_message(type, payload)`, a switch over its messages that own something, and every spawn stores it in the actor's `release_message`; `aether_message_drop()` calls it and frees the payload. Every drop goes through that function:

- the messages queued to an actor that panicked, or that was released, and the ones sent to it after;
- a message of a type its receiver has no handler for (its payload was not even freed before);
- the message a handler panicked on: the step records it in `step_payload` while the handler runs, and the step's panic barrier drops it, since the release after the handler never ran. Its drop functions run in a frame of their own, as the drops at an actor's end do: one that panics is reported through the death hook and ends there (#3403). They run in the step's context, as on the actor's own thread, and the barrier gives the thread's context back after them (#3416);
- a message refused because the scheduler is not running, or that could not be queued for lack of memory.

The receiver's own definitions interpret the payload, as its dispatch does, so a message is released as the program that defined it lays it out. A `ptr` field owns nothing (R4), so a buffer sent with `move` in one and dropped undelivered is not freed; a field of an owned type (`type Buf = owned ptr drop bytes.free`, #3142) is, by this same release, and a handler that binds such a field takes it out of the message, so the release after the handler finds nothing left of it. A message sent through a C `AetherActorRef` whose actor was released has only its payload freed, since the block may hold another actor by then, whose definitions need not be the sender's.

### Asking

An ask (`actor ? Msg {}`) sends its message with a reply slot (`Message._reply_slot`) and blocks on the slot until the handler's `reply` fills it or the timeout passes. When no reply can come, the slot says why and the asker returns at once, not at the timeout (#3151):

- an ask to an actor that is already dead (panicked or released) is not sent;
- an ask whose message is dropped undelivered (its target died or was released on the way, or the mailbox it waited in was drained at the death) fails as dead;
- an ask whose handler panics with it in hand fails from the step's panic barrier as "died before it replied";
- an ask whose handler returns without replying fails when its step ends, as "returned without replying";
- an ask with no memory to make it (its reply slot, or the copy of its message) fails at once and sends nothing (#3387): no state of the thread or of its core is left set, and inside a library call the call ends with `AETHER_OUT_OF_MEMORY`.

The slot rides in the ask's own message, which the ask builds itself (#3395). It is never handed to the send through the thread: the send can step its target inline, and a message that handler sends carries no slot.

The ask a handler holds is part of its thread's step context, with the actor stepping. A step can run inside another (a send steps an idle actor of the same core inline, and a cooperative ask polls until its reply), so each step starts with no ask of its own and gives the enclosing step its context back when it ends: a handler that sends, or asks, before it replies still answers its own asker. The ask is one part of a step's context ("The delivery contract", item 5, has the rest).

`v, err = actor ? Msg {}` reads the reason (the language reference's Ask Pattern lists them); `v = actor ? Msg {}` gives the reply type's zero value for any of them, as it does at a timeout.

An ask made in a handler blocks its core's scheduler thread. Only a core's own scheduler thread does what follows; an actor with a thread of its own sends as a foreign thread and waits without stepping anything for the core (#3246). The core is marked as asking, with the actor it waits on (`Scheduler.ask_target`) and the reply slot (`ask_slot`), and no actor is moved onto it but that one. The waiting thread steps its target itself when the target is the core's (#3220), and relieves the core (#3197): once the wait has lasted `ASK_RELIEF_NS` (1 ms), it does the part of its loop that steps nothing.

- Its deferred sends go out, its own core's through its own queue. A flush already under way on the thread is left to finish (#3247).
- The messages the loop's current batch still holds (`coalesce_buffer.next` marks how far the batch got) go into their actors' mailboxes, for every actor that is not stepping. They are the oldest each has, and the loop skips the entries taken. An actor keeps its entries from the first one that cannot go, so none of its messages passes another. The wait does this at once when its own target is held there.
- Its queued messages go where the loop would put them: into its actors' mailboxes, to an actor's own thread, or on to the core an actor has moved to. A sender's queue is taken in order up to the first message that cannot go now, which the loop delivers once it runs again. That message cannot go when its actor's mailbox is full, or when the batch still holds a message for it.
- Its actors with work (a message, or a receive timeout that is due) and nothing in flight are moved by `move_actor`, the one move path, to a core that runs them. That is the core waiting on that very actor, whose waiting thread is woken through its slot and steps it. When no core waits on it, it goes to a parked core, or else to the core with the fewest messages queued for it that is not asking.

A relief that did something is followed by the next 50 µs later (`ASK_RELIEF_BUSY_NS`). One that found nothing is followed by the next after twice the last gap, up to 8 ms (`ASK_RELIEF_IDLE_MAX_NS`). An actor stepping on this thread (the asker, and any actor below it on the stack) holds its step lock and stays. Nothing is stepped inside the waiting handler: an actor run there that asked the asker could never be answered. Moved to another core, it runs on that core's thread, and an ask of the asker waits there until the asker's handler is done.

An actor with more messages queued than its mailbox holds (32) moves too (#3241). Left in a queue, one of its messages would hold it on the waiting core, and moved without it, its senders' next messages would leave through other queues and could overtake it. So a message the relief finds for an actor whose mailbox is full goes, in order, into an overflow that belongs to the actor: its `inbox`, the unbounded FIFO an actor with a thread of its own takes every message in (#2598), made the first time. There, as in the mailbox, the message is the actor's and no longer counted in flight, and the actor moves with mailbox and overflow together. From then on the overflow holds the actor's newest messages:

- every delivery to it while its overflow holds anything goes behind it: the loop's batch, a local send, a relief, the deferred-send flush, the send buffer;
- every step refills the mailbox from it once the mailbox is down to half, so the mailbox never runs empty while the overflow holds anything, and the loops that step an actor while its mailbox has messages drain both;
- a panic drops what it holds with the mailbox, credited as handled, and a release or a teardown drops and frees it.

Like the mailbox, the overflow is touched only by the thread that runs the actor, and a move hands both over at once. Only a relief starts one: a mailbox found full anywhere else is made room in or queued, as before. The cost on paths that never overflow is one load of `inbox` per delivery and per step.

`tests/runtime/test_ask_relief.c` places the actors on chosen cores. It covers a chain of asks back to the waiting core, the waiting core's other actors, and a same-core target whose message the batch still held; and an actor sent 100 and more messages while its core waits, which moves to another core and handles them all in order, with what is sent to it later, by a local send or through the new core's queue, behind its overflow, and with a panic dropping what the overflow holds.

An ask that was answered or failed returns only once its reply slot is freed. The replier lets go of the slot a few instructions after it wakes the asker, so the asker waits for that and frees the slot itself (#3245). What an ask allocated is gone when it returns, which a program that counts its heap around asks relies on. A timed-out ask does not wait; its replier frees the slot when it comes.

## The delivery contract

What a send promises, on both schedulers (#2974):

1. **Per sender, in order.** Two messages one sender sends one receiver are handled in the order they were sent, on any core and across the receiver's moves between cores (#3090, #3093, #3240, #3241). Nothing is promised between two senders.
2. **At most once.** A message is handled once, or dropped once. A message to an actor that has ended (panicked, killed, stopped), or that its death or a restart in place drops, is released through `aether_message_drop` (what its fields own included) and counted by `scheduler_dead_sends()` (`actors.dead_sends()`); one to a released actor by `scheduler_released_sends()`, one sent while the scheduler is stopped by `scheduler_stopped_sends()`. An ask that no reply can come for fails with the reason as soon as that is known (the actor is dead, dies with it, or its mailbox is full), not at its timeout (#3151).
3. **Unbounded by default, as a decision.** A send never blocks its sender. The sender is often a core's own thread, and one blocked on a full mailbox whose actor that same core, or a core in a cycle with it, must run would deadlock the scheduler. So what a mailbox cannot take waits in the core queues, the senders' deferred sends and the actor's overflow, and memory is the limit.
4. **A bound when the actor asks for one.** `@mailbox(bound: N)` (above, "A bounded mailbox") caps what waits for an actor at N. A send past it is refused at once rather than blocked: counted for a plain `!`, reported to the caller of `try_send` and of an ask.
5. **A handler runs in its own context, whichever thread steps it.** A step can run inside another: a send steps an idle actor of the same core inline, inside the sender's handler, and a cooperative ask polls until its reply. So every step, at entry, puts aside the per-thread context of the code it runs inside and starts with none of it; at exit, normal or by panic, it gives that context back as it was; and its panic, or its end, undoes only what the step itself made (#3389; the rule is written once, in `runtime/actors/aether_step_barrier.h`). What follows the handler is the step's too, and runs in its context as on the actor's own thread: the drop of the message a panicking handler ran on, the finalize of an actor its step ended (the drops of the messages waiting for it are the actor's code) and the death hook, each in a containment frame of its own, so that a panic there ends nothing but itself; the context is given back after them (#3403, #3416). No step runs an actor whose step is already on the stack, so none finalizes an actor whose handler is still running (#3391): both schedulers step an actor only under its step lock, and an ask's poll leaves the asker, and any actor stepping below it, alone. The context, part by part:
   - the ask in hand and the actor stepping: a handler that sends, or asks, before it replies still answers its own asker;
   - the in-step flags the signal handler reads: the outer handler keeps its signal recovery after an inner step;
   - a supervisor's restart claim, the reborn block and the spawn brackets: a nested step's spawn takes no claimed block and is no factory's child, and its panic undoes no enclosing claim;
   - the sandbox levels in force on the thread, in every runtime copy that shares the count: a receiver stepped inside an enforce block is checked as on a thread of its own, against the levels it pushes itself, its drops after its handler included (#3416: they ran against the block's), and the block's levels are in force again when the send returns (#3393);
   - the library calls running on the thread, with what they carry: an interruption on its way, the call's deadline and cancel, the stop a failed allocation notes, and the caller info the host set for the call. An actor stepped inside a call (a send's inline step on a core's thread, or a cooperative ask's poll in an export) is not the call: its loops are not cut by the call's deadline, a `try` in it catches its own panics, and it reads no caller info, as on a core's thread, in its handler and in its drops after it; the call is cut at its own next safe point once the step returns (#3392, #3396, #3416);
   - the execution account in use on the thread (`aether_exec_use`, #2954): an actor stepped inside code that has one in use is charged nothing to it, its drops included, as on a thread of its own; the account stays the thread's meanwhile, and one the step left in use is let go.

   A plain step pays for none of the parts: one word (`aether_caps_slow`'s step bits) says whether some thread holds any of them, read once at each end of the step with one predicted branch; only when it is set are the parts looked at, out of line, each put aside only when this thread holds it. The executions are read from the thread's frames block, which the step's try frame is pushed on anyway. Measured against the barrier before the step-context rule (76d090fd), ping_pong and thread_ring are within 1%, inside the runs' spread; what remains is two thread-local reads at entry, the outer step's in-step flag and actor id, which a nested step gives back.

## The end of an actor

An actor ends once (#2974), by the first of these: a panic in its handler (the step's barrier catches it), `actors.kill(ref)` from anywhere (or the death of an actor it is linked to), `actors.stop()` from its own handler, or a C host's `scheduler_release_actor`. Each sets a bit in `ActorBase.dead` (`PANICKED`, `KILLED`, `STOPPED`, `RELEASED`), and the first keeps its reason with its watch list (`aether_actor_death_reason`): the panic's reason, `"killed"` or `"normal"`. From then on a send to it is dropped.

Its death is then finalized, once, on the thread that owns its mailbox and while the actor steps nothing: what waits for it (its mailbox, the overflow behind it, an actor thread's inbox) is dropped through `aether_message_drop`, an ask among them fails as dead, and every message is credited as handled and counted by `scheduler_dead_sends()` (`actors.dead_sends()`), with those sent to it later and those still on their way when it died. A dropped message's release is the program's code for a field of an owned type (its drop function); each one runs in a frame of its own (`aether_message_drop_at_end`), so a panic in it is reported through the death hook with the actor's id and the panic's reason, as a panic in the release after a handler is by the step barrier, and the drain goes on (#3401: it left the finalize, so the rest of the drain and the notice never ran). The hook runs in a frame of its own too: one that panics is reported on stderr, and the drain goes on (#3417: its panic left the finalize halfway, the messages it had drained never counted, so `scheduler_shutdown` and `wait_for_idle()` waited for them forever, and the watchers never told). Nothing a finalize runs leaves it. Its watchers are then told. Where it happens:

- a panic: in the step barrier, as the step unwinds;
- a stop, and a kill that lands while its handler runs: when the handler returns (the barrier checks the bits after every step). In the barrier, on either path, the finalize runs in the step's context and in a containment frame of its own, before the step gives the thread's context back: its drops are the actor's code, so an actor stepped inline runs them as on its own thread (#3416);
- a kill of an actor that is not stepping: its core finalizes it at the top of its loop, from a request the kill posts (`Scheduler.control`), or from its relief while it waits in an ask. `scheduler_wait` counts the requests, so `wait_for_idle()` returns only once the Downs are sent. Where no core thread runs (a C host between `scheduler_init` and `scheduler_start`, or one that never starts the cores, and a scheduler that has stopped), the kill finalizes it at once, on the killing thread (#3399: before the start it was posted to cores that did not run yet, and never finalized if they never did);
- an actor with a thread of its own: on that thread, at the end of its loop (#3298: its inbox is dropped and credited too, and a sender that pushes after that drain drops what it pushed).

So a `Down` never comes while the actor's handler still runs, and it comes after the last messages the actor sent its watcher, on the same core's path. A killed actor whose handler is waiting in an ask has that ask end at once, with `"the asking actor was killed"`; the handler then runs to its end, and the actor ends after it (a handler is never cut short).

### Death notices

`actors.monitor(ref)`, from a handler, asks for one `Down { ref: actor_ref, reason: string }` when `ref` ends, and returns the monitor's id; if `ref` has ended already, the `Down` is sent at once with the reason it died of. `actors.demonitor(id)` drops the monitor: no `Down` of it is handled after the call, one already on its way included, since the monitor's id rides in the message (`Message.payload_int`) and the generated step checks it against the watcher's own list before the arm runs (`aether_down_accept`). An actor handles it in a `Down(ref, reason) -> { ... }` arm.

`Down` is the runtime's message: the compiler declares it for every program that names it, with the reserved id 254 (`AETHER_MSG_DOWN`), and checks its struct against the runtime's `AetherDownMsg`, since the runtime writes the payload; a program's own `message Down` is refused. Its `reason` is a counted string the message owns. An actor with no `Down` arm drops a `Down` as any message it has no arm for.

Each actor's watchers live in its `death_watch` list (`runtime/actors/aether_supervision.c`), made on first use: who monitors it, whom it monitors, and whom it is linked to. A death marks the list finished under its lock and takes its entries; a monitor that comes after finds it finished and is answered at once. No two lists are locked together.

### Links

`actors.link(ref)`, from a handler, ties the two actors both ways (a `LINK` entry in each list). When either ends, the other takes the death as its watchers are told: an actor that traps exits (`actors.trap_exits(true)`, the `AETHER_EXIT_TRAP` bit) gets a `Down` with the reason (monitor id 0, which `demonitor` never drops), and one that does not ends too, by `KILLED`, with the same reason, unless the reason is `"normal"`, which ends nothing. Its own watchers and links then take its death in turn, so a failure climbs a tree of links until an actor that traps exits. Where a death is finalized as soon as it is seen (the cooperative scheduler, and the threaded one once it is stopped), an end a link passes on while the thread is still making another waits in the thread's queue and is made after it, so a chain of linked actors ends in the same stack depth however long it is (#3394). No panic leaves such an end now (a dropped message's release, an owned field's drop function, and the death hook it reports a panic to each run in a frame of their own, #3401, #3417); the queue is still journaled, so a panic that got out would give up the ends still queued, as an unwinding recursion did, and leave the thread's later links working (#3400). `actors.unlink(ref)` removes the entries both ways; a death already taken stays taken. Linking to an actor that has ended already takes its death at once.

Both schedulers step inside one panic barrier (`runtime/actors/aether_step_barrier.h`), so on the cooperative one too (Emscripten, esp32) a panic is a death with a `Down`. On wasi a panic ends the program, since the target has no setjmp to unwind with, so a death a monitor would watch cannot be caught there: `actors.monitor`, `actors.link` and `actors.trap_exits` (and `std.supervise`, built on them) are refused at compile time on that target, through an `unavailable` attribute on the runtime's declaration.

### Restart in place

A supervisor restarts a child under the ref its siblings and clients already hold (`std.supervise`, built on `actors.rebirth_begin`, `actors.rebirth_end` and `actors.incarnation`). It reruns the child's factory, so the factory's configuration (state writes, first messages) reaches the new incarnation too, and the factory's spawn of the child's actor type takes the child's own block instead of a new one:

1. `actors.rebirth_begin(child)` claims the block when no message for it can still arrive: the child has ended, its death is finalized (what was queued is dropped and counted), its step lock is free (taken now, so nothing steps it), and no message is counted in flight for it. Otherwise it says "not yet" (0), and the supervisor tries again from a message to itself, so it never waits on its own core; or "never" (-1), for a live, released or C-registered actor.
2. The spawn (`scheduler_spawn_actor`, which finds the claim in a runtime thread-local) resets the block: the old state destroyed (`destroy_state`) and zeroed, the mailbox, timeouts, watch lists and reason cleared (the list emptied in place, under its lock: a monitor or a link of the dead child, from another handler, may hold it to read the death it told), `incarnation` one more; `id`, `assigned_core`, the overflow's allocation and the mailbox bound and its pending count kept (every message counted against the bound is taken or dropped, so the count stays exact). The generated spawn then runs the state initializers as on a new block.
3. Its last line, `scheduler_spawn_ready`, clears the death bits with a release store and lets go of the step lock: the child is alive. A fresh spawn is alive already: `aether_rebirth_take` and `scheduler_spawn_ready` read their thread-locals only while a claim or a spawn capture is live on some thread (a process-wide count), so a spawn pays one load for them, not a thread-local read (a call, with emutls).
4. `actors.rebirth_end(child)` returns 1, or undoes a rebirth the factory did not finish (it spawned no actor of that type, or panicked: the child stays ended, "the restart failed"). A panic of the supervisor's own handler undoes it as well.

No message sent before the restart reaches the new incarnation. A counted send reads its target's death after counting itself, so a message counted before the claim held it up (in flight), and one counted after it reads the death and drops itself (counted in `dead_sends`), until the release store, after which it reaches the new incarnation. The main thread's batch flush reads the death only while a claim is live: the claim counts itself in `aether_caps_slow` (from bit 8, the word a step reads anyway) before it reads the child's inflight count, and the flush counts its messages before it reads that count, both seq_cst, so one of the two sees the other. A local send reads the death on the child's core, where the death was finalized. While the child is dead its core touches it only through counted messages (none) and steps it only under the step lock (the rebirth's), so the reset and the initializers run without a race against its core or its senders. A ref need not tell incarnations apart: the handle is the point. `actors.incarnation(ref)` says how many times it was restarted, and a monitor is per incarnation (a client re-monitors after a restart). A C `AetherActorRef` keeps its `id`, so it reaches the new incarnation too.

`tests/runtime/test_actor_rebirth.c` restarts an actor that panics every 150 messages while three sender actors and the main thread send to it: each message is handled at most once and in its sender's order, handled plus dropped is what was sent, nothing reaches a half-made incarnation, and nothing a sender had finished sending when the child was claimed reaches the new one.

### Supervisors

`std.supervise` is an ordinary actor on these pieces (`std/supervise/module.ae`). `supervise.start` runs each child's factory in order before it returns, inside a spawn capture (`aether_actor_capture_begin` and `aether_actor_capture_end`: the first actor that comes alive between the two, through `scheduler_spawn_ready`, is the child; captures nest), so every child's ref is known at once. The supervisor then traps exits and links each child, so a child's death reaches it as a `Down`, and its own death, by a panic or a kill, takes the children with it.

At a child's `Down` it checks the child's restart kind (`"permanent"`, `"transient"`, `"temporary"`) and its budget (the restarts made within the window, kept in a ring of times), then takes the strategy's set: the child alone (`one_for_one`), every child (`one_for_all`) or the child and those started after it (`rest_for_one`). The others in the set are killed, newest first, and each child of the set is marked to restart; one an earlier death marked and that is not restarted yet is not marked again, so a death while a restart still waits joins it, and each child restarts once (#3390). The marked children are restarted from a message to itself, one at a time, the first in start order next: a child it killed waits for its own `Down` (so its death is finalized before the claim), and a claim that says "not yet" is tried again from a later message, so the supervisor never blocks its core. Each restart runs the factory between `rebirth_begin` and `rebirth_end`, in the same capture, and links the child again. Past its budget, or at a restart that fails, it kills its children, newest first, and panics with the reason, so its own watchers get a `Down` saying why, and a supervisor it is linked to restarts it in turn.

## Actor Timeouts

The `after` clause on a `receive` block fires a handler if no message arrives within a given number of milliseconds:

```aether,fragment
actor Monitor {
    receive {
        Heartbeat -> { println("alive") }
    } after 5000 -> {
        println("no heartbeat for 5s")
    }
}
```

The countdown starts when the actor waits on an empty mailbox: at spawn, and again each time it has handled a message. A message that arrives within the interval stops it, and the next wait starts it from the beginning, as Erlang's `receive ... after` does: the Monitor warns every time 5 seconds pass without a heartbeat. The timeout fires once per countdown. When its handler has run, only a message starts the next one, so a handler that sends the actor a message (`self ! Tick {}` below) keeps a cycle going, and one that sends nothing leaves the actor idle (#3162).

Internally, `timeout_ns` in the actor's base holds the interval, set at spawn and kept, and `last_activity_ns` when the countdown was armed (0 while it is not). The generated `spawn_<Actor>` arms it. The generated step function arms it again at the end of each message's handling, disarms it when it takes a message, and before each `mailbox_receive()` fires the `after` arm (disarming it first) when it is armed, the interval has passed by `_aether_clock_ns()`, and no message is waiting. With no message to step it for, the actor's core (or its own thread) steps it once the deadline passes. A core keeps the armed countdowns of its actors in a min-heap of deadlines (#3540): the spawn tells the core of the countdown it arms, the core reads the countdown after each step, and the core parks no longer than until the earliest deadline, so the timeout fires at its deadline, as late as the system's timed wait (a 15.6 ms tick on Windows). C code that arms an actor's countdown by hand, outside its step, tells the core with `scheduler_timeout_armed`. `wait_for_idle()` waits for every armed countdown of a live actor to end (#3102), and for each arm that fired to return (#3443: the step disarms the countdown before the arm runs, and the arm may still send), so a poller that never stops keeps it waiting. The end of `main` waits for the messages sent to be handled, but not for countdowns (#3219): an actor with `after 60000` that handled a message moments before `main` returned does not hold the exit, and its timeout does not run, on either scheduler. `scheduler_shutdown()` calls the wait with countdowns left out, where `wait_for_idle()` keeps them. A program that wants a countdown to fire before it ends calls `wait_for_idle()` first.

The timeout expression can reference actor state fields, not just integer literals, useful when the interval is configurable per-actor or computed:

```aether,fragment
actor Poller {
    state interval_ms = 30000
    state ticks       = 0

    receive {
        Tick -> { ticks = ticks + 1 }
    } after interval_ms -> {
        self ! Tick {}
    }
}
```

Identifiers in the timeout **expression** resolve against actor state fields only. `self` is **not** in scope there: the timeout expression is generated at spawn time (in `spawn_<Actor>`, before `self` exists), so a state-field reference such as `after interval_ms` resolves against the freshly-allocated actor handle. The timeout **body** (above) runs later inside the step function, where `self` *is* in scope, `self ! Tick {}` is fine.

## Panic and Stack Traces

`panic("reason")` unwinds via `aether_panic`. If a `try { ... } catch e { ... }` block (or the scheduler's per-step barrier for actor handlers) is on the stack, the panic is caught and `e` binds to the reason. A reason built at run time (`panic("bad value ${n}")`, a concatenation, a string local) belongs to the catcher: the handler's scope releases it when the handler ends, and whatever keeps it (`kept = e`, a return, a store, a second `panic`) takes a reference of its own, so catching in a loop does not accumulate messages. An actor killed by such a panic releases the reason after its death hook runs. If no frame catches, the runtime prints the reason to stderr along with a filtered stack trace, then aborts:

```text
aether: panic outside any try/catch or actor: divide by zero

Stack trace (most recent call first):
  0: math.safe_divide
  1: calculate_value
  2: main
```

The trace is captured at the call site (codegen wires `aether_panic_capture_stack` in front of every `panic` it lowers) so the user's caller frames survive `-O2` tail-call optimisation. Symbols starting with `aether_` are pretty-printed back to their dotted Aether names, `aether_std_string_concat` reads as `std.string.concat`. User-code symbols pass through verbatim.

The trace is **best-effort diagnostic info**. Under `-O2`, function inlining can fuse callers into one frame; a panic deep inside a chain of small helpers may show only `main`. For richer development-time traces, build with `ae build --quick`, which compiles at `-O0 -g` instead of the default `-O2`; the per-call frames return.

To suppress the trace block entirely (useful for tests that diff stderr line-for-line), set `AETHER_STACK_TRACE=0` in the environment. The reason line still prints.

Available on glibc-Linux and macOS today (uses `<execinfo.h>` `backtrace()` from libc proper, no extra link dependency). On musl, Windows, Emscripten, and freestanding targets the trace block is skipped, `panic`/`try`/`catch` themselves still work.

### Heap cleanup on unwind

A caught panic releases what the scopes it leaves hold, as their exits
would have. While a panic frame is live (a `try`/`catch` block, or the
scheduler's per-step barrier for an actor handler), every place that owns
a value (a string local or temporary, a struct local, an argument the call
must release) is recorded in an unwind journal by its address where it is
declared or first set, and so is every plain `defer` of those scopes.
`aether_panic()` drains the innermost frame's entries before it jumps,
newest first: each place releases what it holds at that moment and is left
empty, and each `defer` runs. `defer try` and `defer catch` do not run on a
panic.

- Stores, moves and releases never touch the journal: the place is recorded
  once, and the drain releases whatever it holds when the panic comes, so a
  value moved elsewhere (returned, stored in a container, actor state or a
  message) is released by its new holder, never by the drain.
- A scope that ends normally forgets its places and runs its `defer`s in
  place.
- A nested `try` drains only the inner frame's entries; the outer frame's
  places are released at their normal exits.
- A place declared outside a `try` and stored inside it is journaled for
  the frame it is declared in (#2690), so a panic the `try` catches leaves
  it to the scope that declared it.

For actor handlers this means a panicking actor's step-scoped values are
released before the actor is marked dead. Outside any frame the journal
calls are a thread-local load and a return, and the no-panic hot path
shows no measurable cost on the ping-pong messaging benchmark. Regression
coverage: `tests/regression/test_panic_unwind_cleanup.ae` (the full
matrix), `tests/integration/panic_unwind_no_leak/` (50000 caught panics,
RSS flat), `tests/integration/panic_actor_step_drain/` (scheduler barrier).

## Cooperative Preemption

By default, message handlers run to completion. A tight compute loop inside a handler will block that core's scheduler thread. For programs where this is a concern, cooperative preemption can be enabled:

- **Scheduler-side**: `AETHER_PREEMPT=1` enables time-based drain loop breaks. After each `actor->step()` call, the scheduler checks elapsed time and yields if the threshold (default 1ms, configurable via `AETHER_PREEMPT_MS`) is exceeded. Cost when disabled: zero.
- **Codegen-side**: `aetherc --preempt` inserts `sched_yield()` calls at loop back-edges in generated C code. A reduction counter (10000 iterations) triggers the yield. Cost when not compiled with `--preempt`: zero (code not generated).

Both levels are independent and can be used alone or together.

## Single-Core Runtime

In single-core mode, actors run cooperatively. The scheduler processes actors in a loop, calling each actor's step function when its mailbox contains messages.

Each step runs inside the same panic barrier the multi-core scheduler uses (`runtime/actors/aether_step_barrier.h`, #2974), so a panic in a handler ends that actor, not the program: its death is finalized on the spot (or when its handler returns, for a kill or a stop that came during it) and its watchers get a `Down`, as above. Where the target has no setjmp (wasi), a panic still ends the program, and watching a death is refused at compile time.

An ask polls the actors until its reply comes, on the one thread. Its timeout holds as with threads (#3330): the polls stop at the deadline, and a handler slower than the timeout, which the poll cannot leave while it runs, has its reply discarded as late, so the ask ends with `"ask timed out after N ms"`. The deadline is the only bound: an ask behind a long queue gets its reply once the queue ahead of it is handled, as with threads. An ask whose message is still queued at its deadline leaves it there as a plain send: it is handled later and its reply goes nowhere. An ask nothing can answer (no actor has a message left and no receive timeout is armed) ends at once with the same reason.

The polls never step the asking actor, nor any actor whose handler is on the stack below it: its next message waits for the handler to return, as with threads (#3391). It used to be handled inside the ask, by a second step of the same actor, its state changing under the first handler, and a `stop()` or a panic there finalized the actor while the first still ran. So an ask only the asker could answer (it asked itself, or an actor that asks it back) is one nothing can answer, and ends at once.

An export of a library on this runtime that asks (or waits) steps the actors it polls on the call's thread, inside the call, and they run outside it, as every nested step does ("The delivery contract", item 5): the call's deadline does not cut their loops and they read none of the call's caller info. A call can only wait for an actor it steps to return; its deadline then cuts it at its own next safe point (#3392).

## Multi-Core Runtime

The multi-core scheduler uses core partitioning with locality-aware placement:

- N cores = N independent scheduler threads
- Each scheduler runs in a thread kept on its core: bound to it on Linux, a hint on macOS and Windows
- Actors placed on the caller's core at spawn time (main thread defaults to core 0; scheduler threads use their own core). This keeps parent-child actor groups co-located for efficient local messaging.
- Cross-core messages use lock-free queues with batch dequeue/enqueue
- Message-driven migration converges communicating actors onto the same core

### Synchronization

The `wait_for_idle()` function blocks until all actors have finished processing their messages (quiescence). It is **non-destructive**: it does not stop or join scheduler threads, so it can be called multiple times in a program. It is for code outside the actors (main, or a host's thread): it waits for every actor, so inside one (a handler, a receive timeout's arm) it would wait for its own step, and on the threaded scheduler a drop or a death hook a core runs would wait for that core; it panics there instead, on either scheduler for a handler or an arm (the cooperative one used to run the other actors nested and return), ending that actor with the reason; a drop run outside any frame ends the process with it, and on the cooperative scheduler a drop or a death hook after a step still waits nested and returns (#3463). This is the recommended way to synchronize the main thread with actor completion:

```aether,fragment
main() {
    ping = spawn(PingActor())
    pong = spawn(PongActor())

    // Send initial message
    ping ! Start {}

    // Wait for all messages to be processed
    wait_for_idle()

    // Actors are now idle, safe to read state
    print(ping.result)

    // Can send more messages and wait again
    ping ! Start {}
    wait_for_idle()
}
```

The implementation uses per-core message counters to detect idle state with minimal overhead on the message-passing hot path. Each scheduler core tracks messages sent and processed locally. The `wait_for_idle()` builtin lowers to the C function `scheduler_wait()`, which sums across cores to determine when all in-flight messages have been handled.

### Initialization

```c
int main() {
    scheduler_init(4);  // Use 4 cores
    scheduler_start();

    // Create and use actors

    scheduler_shutdown();  // Waits for quiescence, stops threads, joins them
}
```

**Scheduler lifecycle functions:**
- `scheduler_wait()`: waits for quiescence (all pending messages processed). Non-destructive: scheduler threads keep running. Safe to call multiple times.
- `scheduler_shutdown()`: waits for quiescence, then stops scheduler threads and joins them. Called once at program exit.
- **Senders the scheduler does not own** (a `std.worker` job, an http pool worker, a C host's thread) must be stopped before `scheduler_shutdown()`: its quiescence wait does not end while one keeps sending. A send from such a thread while the scheduler is not running (shut down, starting again, cleaned up) is refused before it touches a core: the message is dropped, its payload released, the drop counted by `scheduler_stopped_sends()` and reported once on stderr (#2912).

### Core Assignment and Migration

Actors are placed on the caller's core at spawn time. Actors spawned from the main thread default to core 0, keeping top-level actor groups co-located. Actors spawned from within actor handlers inherit the parent's core.

During operation, a sender on another core sets a `migrate_to` hint on the target actor, asking for it on the sender's core. The actor's own core moves it, before it runs the message that brought the request, so a chain of sends (a thread ring, a pipeline) gathers on one core in its first round and its sends turn local. A busy actor whose messages run first is asked again after that batch. A move is non-blocking: if a lock cannot be taken, it waits for the next iteration.

An actor moves only while no message for it is on its way through a core's queues. A sender counts its message in the actor's `inflight` before it reads the actor's core, and the core that delivers the message to the mailbox (or to the actor's own overflow behind it, #3241, which moves with the actor as the mailbox does) uncounts it, a whole run of one actor's messages at once. The move turns `inflight` from 0 to a moving mark, so a message cannot be left behind on the core the actor leaves and then overtaken by the sender's next one (#3093). The same holds for what the moving actor sent itself. A core takes in its messages through one queue per sending core and drains them in index order, so a sender that moved with a message still in a receiver's queue from its old core could have its next one, sent from the new core, delivered first. So a core moves an actor only while nothing sent from that core is still on its way: no core's queue from it holds a message and its overflow is empty (#3240). A message that finds the mailbox full keeps its place: the actor's core steps it to make room, instead of putting the message at the back of a queue. Work stealing goes through the same path: the idle core asks for an actor, and the busy core moves it.

Measured on Windows (i7-13700K, 1,000,000 messages, 10 runs, M msg/s, medians) against the runtime before (#3090): thread ring 24.91 from 3.03, on 0.078 s of CPU from 5.56 s, its 100 actors on one core; fork-join 39.69 from 30.23, each worker's batch delivered as one run; ping-pong (12.56 and 12.36) and counting (42.90 and 42.84) unchanged; skynet 1.69 from 1.31, with a narrower spread.

### Message Routing

- **Same-core messages:** Direct mailbox write (verified via `current_core_id`), behind any message of the actor still in the queues or its overflow (#3241)
- **Cross-core messages:** Counted in the target's `inflight`, then enqueued to the target core's lock-free incoming queue
- **A message on a core the actor has left:** Only one routed by a stale read or by the runtime itself, since a counted message holds the actor where it was routed; forwarded to the actor's current core, still counted

## Memory Management

Actor memory is allocated using NUMA-aware allocation (`aether_numa_alloc_aligned`, on a 64-byte boundary) at spawn time. The `scheduler_spawn_actor` function accepts the full derived-struct size to ensure correct allocation for user-defined actor types.

A typed message's payload travels in the message when it fits (`AETHER_MSG_INLINE`, #3534), else it is a heap copy, freed with `free()` by `aether_free_message` once the message is handled or dropped. There is no payload pool: an actor can move to another core after a message is sent, so a pooled payload would be released by a thread that does not own the pool (#3374).

## Memory Model

The actor runtime uses several allocation strategies for automatic cleanup:
- **Actor memory**: one NUMA-aware, 64-byte-aligned allocation per actor at spawn time (`aether_numa_alloc_aligned`), or the kept block of a released actor of the same size. `scheduler_release_actor` takes the actor out of its core's table and retires it; it is reclaimed once every core's scheduler thread and every per-actor thread has passed the top of its loop since, and no other thread is walking the tables. The scheduler threads, per-actor threads and `wait_for_idle`'s scan for armed timeouts read the tables without a lock, so the reclaim has to wait for them; a core blocked in a long step delays it. A reclaimed block is not returned to the allocator while the scheduler runs: it stays marked released, so a send to it is dropped rather than reading freed memory, and the next spawn of that size takes it. The blocks go back with the scheduler's tables. The mailbox is an inline struct member, not a separate allocation.
- **Message payloads**: a heap copy per typed message, freed when it is handled or dropped
- **Arenas**: a general-purpose arena allocator provides bulk deallocation without per-object tracking for opt-in uses (`std.arena`, JSON parsing). The actor path does not use arenas.

See [Memory Management](memory-management.md) for details on arenas, pools, and allocation strategies.

## Patterns

- **Per-process configuration**: a module-level `var` holds a scalar set once at startup, but it is module-private and not atomic. State shared across actors uses the actor-singleton pattern: spawn one config actor at startup, drive writes via `Set*` messages, drive reads via `Get*` messages that carry an `actor_ref` reply target. See [per-process-config.md](per-process-config.md) for the worked example, the trade-offs (when message round-trip is acceptable vs. when to plumb the value as a parameter or worker-actor cache), and a port of a real C global-state shim.
