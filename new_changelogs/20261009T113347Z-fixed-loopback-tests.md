- **What the test suites start listens on loopback only.** Servers in the
  tests and the swept examples took the std default of every interface, and
  on Windows each new executable that listens there opens a firewall dialog
  and leaves two inbound block rules: one overnight sweep left 56 dialogs
  (about 3.4 GB) and 112 rules. Each now binds `127.0.0.1`, and
  `tests/scripts/check_loopback_listeners.py`, run by `make check-tests`,
  fails on a listener in a test, an example or a run doc block that does not
  name loopback. An HTTP server bound to a host name (`"localhost"`) listened
  on every interface, because the name was never resolved; it now binds the
  address the name resolves to, and a name that does not resolve fails the
  bind. tinyweb's WebSocket and SSE ports listen on the server's host rather
  than every interface, and `std.net` offers `tcp_listen_on_raw` and
  `tcp_server_port_raw` beside `tcp_listen_raw`. The std defaults are
  unchanged (#2639).
