- **The proxy event loop bounds slow clients and the connections it holds
  (#3737, #3750).** On Linux and macOS a server whose work is a reverse
  proxy serves its connections on an event loop, whose idle timer moved
  with every byte: a client that sent one just inside it held a slot for
  ever, and the loop had no bound on how many it held, so enough such
  clients used up the process's descriptors. The loop now holds the worker
  path's budgets, from the same limits: a request head within 30 s of its
  first byte and a body within 30 s plus one second per 500 bytes, each
  answered 408, and a TLS handshake within its 30 s. It holds at most
  `http.server_set_loop_max_connections` connections, by default half the
  process's open-file limit less 64; at the bound it closes the connection
  that has waited longest with nothing of a request sent, and answers a new
  one 503 when there is none. A refusal is followed by a short lingering
  close, so a reset does not destroy it.
