- **A registration racing a core's take of the actor leaves what the core
  set (#3673).** `scheduler_register_actor` reset the actor's overflow,
  ready list byte and timer slot before its compare-and-swap, while a core
  that had just taken the actor could have set them: the byte was cleared
  under the core's entry, and an overflow the core had started was
  dropped. Nothing is reset there now; teardown and the deregister leave
  them as a registration needs them.
