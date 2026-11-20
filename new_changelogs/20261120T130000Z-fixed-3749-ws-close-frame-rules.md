- **WebSocket close frames keep to RFC 6455 5.5.1 and 7.4 (#3749).** A close
  frame from the peer with a one-byte body, or with a code no close frame
  may carry (below 1000, 1004 to 1006, 1015 to 2999, 5000 and up), now
  closes the connection with 1002; both were echoed back as they came.
  `ws_close` (and `http_ws_close` in C) now returns `""`, or why it sent
  nothing: a code a close frame may not carry, or a reason that is not
  UTF-8. A reason past 123 bytes is cut at a whole code point, where it was
  cut at byte 123 even inside one. **Behaviour change:** `ws_close` with
  such a code or reason used to send the close frame anyway and mark the
  connection closed; it now sends nothing, returns the reason, and leaves
  the connection open, so a caller that ignored the result and went on as
  if it were closed should check it and close with an allowed code (1000
  for a normal close).
