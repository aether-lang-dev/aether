- **`std.http1` no longer reads a body after a 1xx, 204 or 304 response,
  or after a response to HEAD (#3520).** RFC 9110 says such a response ends
  at its header section whatever its headers say, and `std.net` reads it
  that way, but the reader took `HTTP/1.1 204` with `Content-Length: 5` to
  have a body of five bytes, and waited for the body of a 304 or of a HEAD
  response, or for the connection to close. It now completes them with an
  empty body. The reader cannot tell a response to HEAD from the bytes, so
  `http1.response_new_for(method)` makes a reader that knows the request's
  method.
