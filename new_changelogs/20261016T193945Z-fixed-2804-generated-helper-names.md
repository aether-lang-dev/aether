- **A function may be named like a generated helper.** A program function
  called `Ship_dup`, `Note_release_fields` or `Counter_step` collided with
  the helper the compiler generates for that struct, message or actor, and
  the C did not compile. The helpers are in the reserved `_aether_`
  namespace now; a function named `spawn_<Actor>`, which a C host calls, is
  an error naming the actor (#2804).
