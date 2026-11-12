- **std.net: a server's running flag is atomic (#3676).** `http_server_stop`, on any
  thread, cleared `is_running` while the server's accept loops read it on
  theirs, with nothing ordering the two: a data race on every stop of a
  running server, which ThreadSanitizer reported in 27 test programs
  (`make test-tsan`, #3542). The flag is an atomic int now.
