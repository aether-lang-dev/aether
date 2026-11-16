- **Actors: a send from a core to an actor just moved onto it orders its
  mailbox write after the previous core's (#3683).** The fix for #3683 made
  the ownership check of `send_local_admitted` an acquire, but the direct
  delivery in `send_through_core`, which a death notice and every remote
  send from a core take, still read the actor's core relaxed before writing
  its mailbox. ThreadSanitizer reported it in the C runner and
  `test_supervise` (`make test-tsan`, #3542). It is an acquire too now.
