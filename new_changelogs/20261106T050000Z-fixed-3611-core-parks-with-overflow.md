- **A scheduler core with messages still to deliver no longer parks (#3611).** When a target core's queue from a core is full, that core keeps the rest of what it sends in its overflow buffer, which only its own thread flushes, once per loop turn.

  A core that then ran out of actors to step parked after its idle spin. The check before parking looks only at the queues coming in, so the buffered messages waited until the park ended: up to 32 ms each time. Meanwhile:
  - the target's core sat idle with messages owed to it;
  - `scheduler_wait` waited for them;
  - a dead actor's restart in place waited on its count of messages in flight (#3606).

  Now a core whose buffer holds anything yields the CPU instead of parking, so a target core that shares the CPU can make room, and it flushes again on its next turn. A core with an empty buffer parks as before.

  A new runtime test sends 3,072 messages from core 1 to an actor on core 0 while core 0 is held in a step, then lets core 1 idle with the cores' park set to a minute. Every message must arrive within 10 s. Before the fix 1,023 arrived.
