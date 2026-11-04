- **Allocating from many threads no longer fights over one counter
  (#3529).** Every cap-aware allocation and free of every thread counted
  its bytes on one global atomic, even with no memory cap set, so building
  strings on eight actors took 95 ns a string against 65 on one. Without a
  cap each thread now counts on a cache line of its own, summed when the
  total is asked for; eight workers take 12.9 ns a string (best of 5, this
  machine), one worker is unchanged. Setting a cap moves the threads'
  counts into the one counter the capped path checks exactly, so a cap set
  later still sees the memory already in use.
