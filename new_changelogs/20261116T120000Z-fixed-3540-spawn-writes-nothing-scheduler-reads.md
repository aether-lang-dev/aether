- **A spawn no longer writes the actor fields a core may read once the
  actor is in its core's table (#3540).** The generated `spawn_<Actor>`
  set `active`, `migrate_to` and `auto_process` again after
  `scheduler_spawn_actor` had set them and placed the actor, while a core
  that moved another actor away could move this one into the hole and read
  them (ThreadSanitizer). It now writes only the actor's state and what
  differs, and a restart in place writes nothing a sender may be reading.
