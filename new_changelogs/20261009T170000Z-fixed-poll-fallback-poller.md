- **An idle HTTP server on Windows no longer spins a core, and its
  keep-alive connections are watched as soon as they are parked.** The
  poll() fallback poller, which is what Windows runs (WSAPoll), returned at
  once from a wait on an empty set, so the server's parking-lot thread
  spun: an idle background server used 2.98 CPU seconds in 3 seconds, and
  now uses none. It was also not safe for the lot's workers to register a
  connection while the lot's thread waited (a registration could reallocate
  the array the wait was reading), and a connection registered during a
  wait was only watched from the next one, up to 200 ms later. The set is
  now guarded, a wait polls a copy of it plus a wake-up socket of the
  poller's own that a registration signals, an empty wait lasts its
  timeout, and a result is reported only for the registration it was
  polled for (#2679).
