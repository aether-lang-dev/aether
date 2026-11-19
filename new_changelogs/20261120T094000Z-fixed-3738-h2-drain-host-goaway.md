- **A host that serves its own connections can speak HTTP/2 on them
  (#3738).** The HTTP/2 loop sent GOAWAY once the server was not running,
  and a host that hands connections to `http_server_drain_connection` never
  starts its server, so every HTTP/2 connection it served got a GOAWAY
  before its first request, which was never answered. The loop now keys on
  the stop itself, which `http_server_stop` signals.
