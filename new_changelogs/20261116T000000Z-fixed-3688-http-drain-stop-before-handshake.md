- **std.net: a connection taken after the stop is closed before any TLS
  handshake (#3688).** `http_server_drain_connection` did the TLS handshake
  before its first check of the stop, and the handshake reads from the
  client with no time limit. A worker actor queued behind a stop (on its own
  core, or a one-core scheduler) and stepped after the stop had returned
  shook hands with a slow client and held its core meanwhile, where a plain
  connection was closed at its first read. The drain now closes the
  connection unserved as soon as it finds the server stopping, before the
  event-loop handoff and the handshake, whichever backend terminates TLS.
  Covered by the stop-on-core probe in
  `tests/integration/http_actor_mode_release`, now run plain and over TLS
  through each backend the build has, with a client that stalls after the
  first bytes of its handshake.
