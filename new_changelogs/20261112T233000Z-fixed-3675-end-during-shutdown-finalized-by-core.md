- **An actor ended while `scheduler_shutdown()` stops the cores has its
  death finalized by its core (#3675).** A kill, a link's death or `scheduler_actor_end`
  from another thread after the shutdown had left the running state, but
  before the core threads were joined, finalized the death on the calling
  thread at once: the actor's mailbox drained and its watchers told while
  its core could still be stepping it (a receive timeout's arm, which the
  shutdown does not wait for). Its core now finalizes it once the step
  returns, or as its thread leaves its loop; a core that has left its loop
  refuses the request, and the caller finalizes the death as before.
