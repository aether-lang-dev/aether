- **std.net on Windows: a stop wakes the server's accept loop instead of
  closing the socket under it (#3677).** Windows was left out of #3677's fix:
  its accept loop blocked in `accept()`, which only closing the listening
  socket ended, so the stop still closed it under the loop, and the loop's
  next `accept()` could take a socket another thread opened under the same
  number. The loop now waits in `WSAPoll` on the listening socket and a
  loopback wake socket of the server's, a stop sends the wake socket a byte
  and leaves the listening socket open, and the run closes it once its loop
  is over, as on POSIX. `http_server_stop` still returns once the run has
  stopped.
