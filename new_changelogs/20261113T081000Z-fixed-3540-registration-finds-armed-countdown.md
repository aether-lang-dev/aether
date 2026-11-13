- **A caller's actor keeps its armed receive timeout across a restart, and
  one armed before its registration fires (#3540).** A core looks only at
  the actors it is told of; an actor carried over a scheduler restart lost
  its timer heap entry with the last lifecycle's tables, and one armed
  before its registration was told while it was on no core. The
  registration now tells the actor's core to look at it once, as the old
  scan did on its next pass.
