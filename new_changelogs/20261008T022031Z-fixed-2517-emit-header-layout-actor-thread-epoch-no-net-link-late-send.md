- **The `--emit-header` file declares the message structs the generated C
  uses (#2517).** It listed a message's fields in declaration order while the
  .c packs ints first, then pointer-sized fields, then the rest, so a C host
  built against the header wrote every field but the first of an interleaved
  message at the wrong offset. Both are now written from one field order.
  The header had also been empty since #996 gated its contents on the
  `--emit=csrc` catalog header, and its typed send helper sent a multi-field
  message with no payload; it now builds the struct and sends it through
  `aether_send_message`, and an inline one through `payload_int`, as the
  generated code does.
- **A per-actor thread takes part in actor reclamation (#2517).**
  `aether_actor_thread` sets the core id, so a send from its actor's step to
  another actor of the core stepped that actor on the actor thread, which
  published no epoch: a release made in that step freed the actor while the
  thread was still touching it. The thread now publishes an epoch like a
  core, refreshed at the top of its loop, and leaves the loop once its own
  actor is released, ending the actor itself; a release of such an actor
  from any thread only marks it and never waits.
- **The no-networking build links as a shared library on Windows (#2517).**
  The HTTP worker pool and parking lot were built without networking and
  referenced server functions the stubs do not define, and the proxy
  referenced client helpers in the same state. The pool and the lot are now
  built only with networking; the client's clock and header validators,
  which need none, are built always; the two client-bound helpers the proxy
  links against are stubbed.
- **A send to a released actor is defined (#2517).** A reclaimed actor's
  memory is no longer returned to the allocator while the scheduler runs: it
  stays marked released, in a bucket for its size, and the next spawn of that
  size takes it. A send that reaches it is dropped, counted
  (`scheduler_released_sends`) and reported once on stderr instead of
  reading freed memory; a send made after the block has become another
  actor reaches that actor. The send fast path is unchanged: it already read
  the dead mark first. The cooperative scheduler drops such sends the same
  way; it used to deliver them.
