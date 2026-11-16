- **A core's spin before a park reads no clock for its first 1,024 passes
  (#3540).** The spin, counted in time since #3540, read the clock on the
  first idle pass of every pause, so each ask from a thread no core owns
  paid a clock read between the reply and the pickup of the next ask. On a
  clock source that is slow to read, as a VM's can be, that doubled
  ask_from_main on CI. The clock is read once a pause outlasts the first
  1,024 passes, then every 64 as before.
