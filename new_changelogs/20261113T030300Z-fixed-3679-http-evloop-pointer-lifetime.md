- **std.net: the proxy event loop is never read after the server frees it (#3679).**
  The server's thread set and cleared its event loop pointer and freed the
  loop while `http_server_evloop_wakeups`, `_active` and `_connections` read
  it on the program's thread, and while the pool's workers could still be
  handing it a connection: a data race that ThreadSanitizer reported in two
  proxy tests (`make test-tsan`, #3542), and a read of freed memory when a
  reader lost the race to the free. The pointer is behind a lock of its own
  now; the server clears it there before it stops and frees the loop.
