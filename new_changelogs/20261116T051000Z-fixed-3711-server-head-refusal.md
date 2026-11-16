- **std.http: a refusal to a HEAD carries the head only (#3711).** The
  server's canned refusals of a request it will not read (431 for a header
  line or a head too large, 414 for a request line too long, 413 for a
  chunked body past its bound, 400 for framing it will not guess at or a
  request that does not parse) were sent whole, body included, whatever the
  method, so a HEAD got a body that a client reads as the start of the next
  response (RFC 9110 9.3.2). Every refusal now goes through one helper that
  sends the head only when the request's bytes say it is a HEAD, keeping
  its Content-Length; one sent before the method has arrived goes out whole,
  as before. Covered by `tests/integration/http_server_head_refusal`.
