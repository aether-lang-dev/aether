- **The HTTP client refuses a content type with a line ending in it.** The
  content type given with a request body went into the request head as
  `Content-Type: <value>` with no check, while every other header value was
  checked for CR and LF. An application passing a caller-supplied content
  type to `http.post`, `http.put` or `client.set_body` let a user add headers
  to the request, or smuggle a second request to the upstream. `set_body` now
  refuses it, and `http.post`, `http.put`, `client.post_with_status` and
  `client.patch` return an error instead of sending (#3741).
