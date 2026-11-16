- **A caller's actor carried over a scheduler restart keeps one core
  delivering to it (#3673).** Sends from a thread no core owns, made after
  `scheduler_init` and before `scheduler_register_actor`, went to the core
  the actor's last lifecycle left it on, and that core delivered them; the
  registration then gave the actor the least loaded core, and both cores
  wrote its mailbox. A message for an actor whose core was none of the
  lifecycle's was also delivered by whichever core took it. Such an actor
  now keeps the core that delivers to it (a core asked for is asked of
  that core as a move), and a core that meets an actor with no core takes
  it before it delivers. In the new runtime test, a foreign thread sends a
  sequence to a carried-over actor across 20 restarts: before, 45,000 to
  270,000 messages arrived out of their order; now none.
