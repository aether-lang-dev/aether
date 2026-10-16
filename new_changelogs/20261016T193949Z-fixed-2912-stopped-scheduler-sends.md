- **A send from another thread while the scheduler is stopped is refused.**
  A `std.worker` job, an http pool worker or a C host's thread sending while
  the scheduler was shut down, starting again or cleaned up could take a
  core's lock between its destroy and its init, or divide by a core count of
  0. Such a send is now dropped, its payload released, counted by
  `scheduler_stopped_sends()` and reported once (#2912).
