- **std.http.proxy: a HEAD through the event loop gets the upstream's head
  only (#3710).** The proxy event loop on Linux and macOS passed an
  upstream's answer through from its receive buffer, all of it, so an
  upstream that sent a body with its answer to HEAD had that body relayed,
  and the client read it as the start of the next response. The loop now
  ends a response where its own framing says (an answer to HEAD at its
  head, as the worker path's client already did), on the pass-through path
  and the copying one, keeps the upstream's Content-Length on an answer to
  HEAD (RFC 9110 9.3.2 lets it state the GET's length), closes the client's
  connection after the head when the upstream stated no length, and does
  not pool an upstream connection that sent bytes past its response.
  Covered by `tests/integration/http_proxy_loop_head_no_body`.
