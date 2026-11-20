- **The reverse proxy answers an upstream's response the same way on both of
  its paths (#3749).** Found by the new proxy-response fuzz target, which
  holds the copying path and the event loop's pass-through to the same head:
  - an upstream that closed with no response head, or sent bytes that are
    not HTTP, was read as status 0 with no error and the client was sent
    `HTTP/1.1 0`; the HTTP client now reports it as an error, so the proxy
    answers 502 (and `http.get` no longer hands back status 0 as a reply);
  - the pass-through answered a response that had an interim (1xx) one ahead
    of it with the interim's status; it leaves those to the copying path;
  - the copying path kept only the last of a repeated field, so of several
    `Set-Cookie` fields the client saw one; each is passed on;
  - a response held at most 50 header fields and dropped the rest silently,
    so a proxied response with more lost them; the fields now grow as
    needed;
  - the pass-through wrote a repeated `Content-Length` once a line; it
    writes it once.
