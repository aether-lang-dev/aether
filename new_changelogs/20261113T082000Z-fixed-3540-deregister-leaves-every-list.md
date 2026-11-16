- **A deregistered actor is out of every ready list and timer heap when
  `scheduler_deregister_actor` returns (#3540).** Its countdown stayed in
  its core's heap: freed at once, the block was read when the countdown
  came due; registered again, its old core stepped it while it was another
  core's, or spun forever on an entry its slot no longer named. The
  deregister now waits for its core to drop the actor, and a core fires
  only countdowns its heap and the actor agree on.
