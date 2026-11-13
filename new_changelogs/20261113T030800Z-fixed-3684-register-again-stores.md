- **Actors: registering a C caller's actor again while another thread sends
  to it is free of a data race (#3684).** `scheduler_register_actor` reset the
  actor's mailbox bound and its counts with `atomic_init`, which is a plain
  store, while a thread of the caller's could be sending to the actor and
  reading them; ThreadSanitizer reported it in `sched_foreign_send_lifecycle`
  (`make test-tsan`, #3542). They are reset with atomic stores now.
