- **std.net: the one-time start of the network layer is free of a data
  race (#3680).** A server made on one thread (an actor's) while a client connected
  from another both read and set a plain flag that says Winsock is started;
  ThreadSanitizer reported it in `http_ipv6` (`make test-tsan`, #3542). The
  flag is atomic; two threads that both find it clear both start Winsock,
  which counts its starts.
