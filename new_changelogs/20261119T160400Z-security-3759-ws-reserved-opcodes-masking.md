- **WebSocket frames with a reserved opcode, or the wrong masking, close
  the connection with 1002 (#3759).** The frame reader appended a frame with
  a reserved data opcode (3 to 7) to the message as if it continued it, and
  took one with a reserved control opcode (0xB to 0xF) as data, where RFC
  6455 5.2 requires failing the connection. A server also accepted unmasked
  frames from its client, which RFC 6455 5.1 requires it to refuse: masking
  keeps a client's script from choosing the bytes an intermediary sees, the
  defence against cache poisoning through a WebSocket. Both now close with
  1002, and so does a client handle from `http.ws_connect` that receives a
  masked frame from its server, the other half of 5.1. Every conforming
  client masks, `ws_connect`'s handles included, so only a peer that broke
  the protocol is refused.
