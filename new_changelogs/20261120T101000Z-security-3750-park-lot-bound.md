- **Idle keep-alive connections past the parking lot's bound no longer hold
  workers (#3750).** The worker path waits for a connection's next request
  in a parking lot rather than on a worker, but the lot held a fixed 4096,
  and a connection the full lot declined went back to waiting on its
  worker: idle clients past the bound held the workers one each. The bound
  is now `limits.park_max_connections` (`http.server_set_park_max_connections`),
  by default the event loop's (half the process's open-file limit less 64;
  4096 on Windows), and at it the lot closes the connection idle longest to
  make room.
