- **std.net: `http_server_port` reads the port the server's thread binds
  without a data race (#3678).** A server started in the background with no bind of
  its own binds on its thread, while the program asks `http_server_port` on
  another; the port was a plain int written there and read here, which
  ThreadSanitizer reported in six programs (`make test-tsan`, #3542). It is
  an atomic int now, written once with the port as bound.
