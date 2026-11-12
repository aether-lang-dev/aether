- **std.net: in actor dispatch mode a stop waits for the workers still
  serving.** A server that hands each connection to a worker actor ended
  its run as soon as its accept loops were over, with workers still in a
  request, so `http_server_stop` returned and the program's
  `http_server_free` freed the server while a worker still read it.
  ThreadSanitizer reported the free against a worker in
  `http_server_multi_accept` (`make test-tsan`, #3542). The run now waits,
  as the thread pool's does, until every worker given a connection has
  finished with the server.
