- **A core parks while a move it was asked for waits for room on the
  destination core (#3540).** A move to a core whose table is full cannot
  be made until that table grows, and the core asked for it again on every
  pass, never parking. It asks again when the actor next has work.
