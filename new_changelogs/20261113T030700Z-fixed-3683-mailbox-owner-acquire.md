- **Actors: a core that has just been handed an actor sees its mailbox as the
  previous core left it (#3683).** A send from a core checked that the target was
  its own with a relaxed load of the actor's core, then wrote the mailbox,
  whose one writer is the core that has the actor. Right after a move, that
  read did not order the new core's write after the old core's last
  delivery: on a weakly ordered CPU the new core could read a stale tail and
  overwrite a message. ThreadSanitizer reported it in `test_supervise` and
  std.stream's test (`make test-tsan`, #3542). The check is an acquire load
  now, pairing with the move's release store; so is the overflow flush's.
