- **A streamed response to HEAD, a 204 or a 304 ends at its headers
  (#3551).** `send_stream` framed a response by its `Content-Length` and
  `Transfer-Encoding` alone, so a streamed HEAD answered with a length, or a
  304 with one, had `response_read` wait for a body until the request's
  timeout, and a 204 without a length waited for the connection to close.
  The stream reader now keeps the rule the buffered reader has: a response
  to HEAD, a 1xx, a 204 and a 304 carry no body (RFC 9110).
