- **Actors: a message dropped for a dead actor no longer reads the actor after
  its block can be reused (#3682).** A core that found a message for an actor that
  had died gave back the message's count in flight, then released the
  message through the actor's own release function. The count is what keeps
  a released actor's block from a new spawn, so a spawn in between could
  re-initialize the block under the drop; the core also looked at the dead
  actor's entry again later in the same pass. ThreadSanitizer reported both
  (`make test-tsan`, #3542). The count is given back after the drop now, at
  every place that drops one, and the core forgets the entry.
