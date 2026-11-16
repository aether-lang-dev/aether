- **A proxied or cached response keeps the upstream's last header field
  (#3714).** The client keeps a response's header block cut at its blank
  line, so the last field has no CRLF after it, and the proxy's copy of the
  upstream's headers and the cache's snapshot of them took a line only when
  it ended in CRLF: every response through the proxy middleware or from its
  cache lost whatever field came last (an `ETag`, a `Set-Cookie`, a
  `Location`). One line splitter now reads a header block for all three.
