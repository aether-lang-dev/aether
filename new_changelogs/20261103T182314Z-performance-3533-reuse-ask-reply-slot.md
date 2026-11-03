- **An ask reuses its thread's reply slot instead of building a new one
  (#3533).** Every ask allocated a reply slot and created a mutex and a
  condition variable for it, then destroyed and freed all three when it
  returned. Slots are now made once and lent: an ask takes its thread's
  last slot when both sides have let go of it, and another when a timed
  out ask's slot is still held by the actor. An ask between two actors on
  one core went from 210 to 190 ns on this machine (best of 5); across
  threads the wake up dominates and the change is within noise.
