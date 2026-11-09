- **`http_server_stop` waits for a foreground server too, so a free after it
  is safe.** Stop waited only for a server started with
  `http_server_start_background_raw`. A foreground `http_server_start_raw`
  run on another thread (an actor's) was signalled and left running, and the
  `http_server_free` that followed released it under its accept loop and a
  pool worker still serving a keep-alive connection: a segfault in
  `conn_recv_request_start` / `http_resolve_route` on macOS and Linux (found
  by servirtium-vcr). Now a stop waits until the run is over, whichever start
  began it. On one of the server's own threads (a handler, the monitor, an
  HTTP/2 dispatch worker, the proxy driver's middleware) it only signals,
  since the run waits for that thread: a background server stopped from a
  handler used to hang there. `http_server_free` on such a thread while the
  server runs is a fatal error rather than a use after free, and a start of
  a running server is refused with the reason in `http_server_error`.
