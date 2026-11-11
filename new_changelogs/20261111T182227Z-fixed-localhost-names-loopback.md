- **`*.localhost` names are loopback on every platform, with no DNS query.**
  `app.localhost` said no such host on Windows and FreeBSD (FreeBSD after
  asking its DNS server), and a Linux host without systemd-resolved's stub
  sent the query to its DNS server too, so a client and a server naming a
  service by it met only on macOS and most Linux desktops. std.net's dial,
  the HTTP client (and its proxy event loop's resolver), the WebSocket dial
  and the HTTP server's listen now answer a name ending in `.localhost`, and
  the rooted `localhost.`, as `::1` then `127.0.0.1` (a listener binds
  `127.0.0.1`), as RFC 6761 6.3 asks. Plain `localhost` still goes to the
  system resolver, which answers it from its hosts file.
