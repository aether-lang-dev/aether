- **`scheduler_deregister_actor` waits for nothing again, and a caller may
  still free the actor once it returns (#3540).** It waited for the actor's
  core to drop the actor's ready list and timer entries: for the length of
  an armed receive timeout (a minute's took a minute), forever on the
  actor's own core, and two cores whose steps deregistered each other's
  actors deadlocked. Each entry now names the table slot its actor was at,
  and a core reads the actor only while its table holds it there, so the
  deregister just leaves the table, in microseconds.
