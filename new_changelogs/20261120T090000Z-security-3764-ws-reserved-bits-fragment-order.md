- **WebSocket frames with a reserved bit set, or fragments out of order,
  close the connection with 1002 (#3764).** std.http negotiates no
  WebSocket extension, so RSV1 to RSV3 must be clear (RFC 6455 5.2), but a
  frame with them set was read as if they were not: a peer that believed it
  had permessage-deflate had its compressed bytes handed to the program as
  the message. A continuation frame with no message in progress was taken
  as a message of its own, and a text or binary frame inside a fragmented
  message restarted it and dropped the fragments before it (5.4). All three
  now fail the connection with 1002, on server handles and on
  `http.ws_connect`'s alike. Control frames between fragments are still
  allowed, as the RFC says.
