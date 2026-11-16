- **A message-driven move finds its actor by the slot it keeps (#3540).**
  Each actor keeps its slot in its core's table, and a move asked through
  the ready list or on delivery checks it before it scans. It walked the
  whole table under both cores' locks: with 100,000 actors on each of two
  cores, a round that moves an actor took 24.8 us, and 0.9 us now, as with
  1,000 actors.
