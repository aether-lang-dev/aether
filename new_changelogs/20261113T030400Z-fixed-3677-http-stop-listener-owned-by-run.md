- **std.net: stopping a running server no longer closes the listening socket
  under its accept loop.** `http_server_stop` closed the socket while the
  server's thread could still be about to `poll()` or `accept()` on its
  number, which by then the system could have given to a socket or file
  another thread opened: the loop would wait on, or accept from, that one.
  ThreadSanitizer reported the unguarded read of the number in six programs
  (`make test-tsan`, #3542). On Linux, macOS and the BSDs a stop now only
  shuts the socket down (the wake pipe ends the loop), and the run closes it
  once its loop is over; with no run on, the stop closes it as before. A
  bind publishes the socket under the server's lock, where a stop reads it.
