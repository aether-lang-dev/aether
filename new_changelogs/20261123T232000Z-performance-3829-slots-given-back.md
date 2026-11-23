- **A thread that ends gives its memory-accounting slot back.** Each thread
  that counts owns a slot it updates without a locked add (#3829), but
  slots were never given back: once 63 threads had ever counted, every later
  thread shared the last slot through a locked add, so a server with a
  thread per connection, or a worker pool made again and again, contended on
  one cache line. A thread now folds its slot's count into the exact total
  as it ends, so nothing it counted is lost, and gives the slot back for the
  next thread to take.
