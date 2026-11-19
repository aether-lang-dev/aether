- **`http.request_body` refuses a body past the server's limit.** It
  buffered whatever Content-Length the client declared, up to `LONG_MAX`,
  allocated before any of it arrived, so a few connections each declaring a
  multi-GB body took the server's memory. The server now has a limit on a
  buffered body, 64 MiB by default and set with `http.server_set_max_body`.
  Past it `request_body` returns `""` without reading the body,
  `request_body_length` reports 0, and the server answers 413. Under it the
  buffer grows as the bytes arrive rather than at the declared length.
  `request_body_read` streams a body of any size and is not bounded by the
  limit, so large uploads keep working (#3740).
