- **std.net: the HTTP client's and the socket layer's once-flags are atomic
  too (#3680).** #3680 made the HTTP server's start-Winsock-once flag atomic;
  the HTTP client (`http_init`) and the socket layer (`aether_net_init`)
  each kept a plain copy of it, which two threads making their first
  request or socket at once raced. ThreadSanitizer reported the client's in
  two proxy tests (`make test-tsan`, #3542).
