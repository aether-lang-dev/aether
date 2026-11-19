- **An HTTP server out of descriptors no longer spins on its listener.**
  When the process ran out of descriptors (or memory), accept() failed with
  the connection still queued and the listener still readable, and both
  accept loops went straight back to poll and accept: a core spun for as
  long as a client held its connections open. The loops now stop watching
  the listener for 100 ms after such a failure, as nginx disables its
  accept events, and the queued connection is served once a descriptor
  frees.
