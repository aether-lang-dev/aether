- **A WebSocket peer can no longer make the server hold what it only
  declares.** The receive allocated each frame at the length it declared,
  up to 1 GiB, before any of it arrived; reassembled fragments with no total
  bound, in an `int` capacity that overflowed past 1 GiB and then looped;
  and echoed pings and closes of any size, fragmented or not. A message now
  has a cap, 64 MiB by default, checked against each frame's declared length
  before it is read and closing with 1009 past it:
  `http.server_set_ws_max_message` sets it for the handles a server
  upgrades and `http.ws_set_max_message` for one handle. A frame's bytes go
  into a buffer that grows as they arrive, sizes are `size_t`, and a control
  frame over 125 bytes or fragmented closes with 1002 (RFC 6455 5.5). A
  client handle from `ws_connect` has the same 64 MiB cap (#3739).
