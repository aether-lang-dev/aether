- **`mem.steady_growth` no longer reports a round's own thread as a leak.** A
  run that leaves the heap exactly as it found it now answers 0 even when a
  thread started or ended during it: a leak grows every run, and a thread's
  blocks only add to the count. A round that starts and joins a thread of its
  own (a proxy pool's health-check probe) changed the thread count on Windows
  every run, so no run was compared, and the answer was whatever the last run
  happened to grow. `proxy_mount_frees_with_server` failed about one run in
  three on Windows that way, with no leak: 200 rounds grow the heap by 3000
  bytes once, then not at all (#3730).
