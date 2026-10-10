- **A std.http server stopped before its thread starts stops, at once.**
  `http_server_start_background_raw` returned as soon as it spawned the
  server thread, whose first act was `is_running = 1`: a stop issued in
  between was overwritten, the accept loop then ran for good, and since
  0.799 (#2672) `http_server_stop` joins that thread, so open, start in the
  background, stop with no request in between hung the program (found by
  servirtium-vcr's "close is idempotent" check). The start now marks the
  server running before it spawns the thread, and the thread runs nothing
  if a stop came first. The poll loop also watches a wake pipe of the
  server's own that a stop writes to: macOS and the BSDs do not wake a
  `poll()` on a socket that is shut down, so each stop there waited out the
  loop's one-second timeout. `tests/integration/http_server_stop_before_start`
  runs 50 open/start/stop cycles under a watchdog.
