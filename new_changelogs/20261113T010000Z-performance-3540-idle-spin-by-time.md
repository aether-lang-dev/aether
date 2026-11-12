- **A scheduler core spins for a time, not a count of passes, before it
  parks (#3540).** The idle spin was 10,000 loop passes, set when a pass
  read every actor of the core. With the ready list a pass costs the same
  with ten actors or a million, so the spin ended sooner, and on a machine
  whose pause instruction is short much sooner: a pause between two
  messages longer than it parks the core, and the next message pays the
  park and its wake. A core now spins for 2 ms (`AETHER_IDLE_SPIN_NS`),
  about what the 10,000 passes took with a few actors per core on an x86
  desktop, reading the clock every 64 passes. An idle program still parks
  its cores: over 5 s of idleness it used 0 to 16 ms of CPU before and
  after.
