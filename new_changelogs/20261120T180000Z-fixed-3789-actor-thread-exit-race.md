- **An actor thread's exit no longer writes the actor after marking itself
  gone.** When its death was already final, it ran the once-only finalize
  test again after the mark that lets another thread free the actor, and
  that write raced the free (TSan, #3789).
