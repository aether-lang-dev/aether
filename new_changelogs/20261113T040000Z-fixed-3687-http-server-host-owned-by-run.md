- **std.net: a server's host is not freed under its own run (#3687).** A server
  started with no bind of the program's own binds on the thread that runs it,
  and that bind freed and replaced `server->host` with no lock, while the
  program could set the host (or bind) on its own thread, freeing and
  replacing it too: two frees of one string, or a read of a freed one. A run
  now binds and reports (to the monitor) a copy of the host made as it
  starts, under the server's run lock, and every change of the host is made
  under that lock. A host set while the server runs takes effect at the next
  start.
