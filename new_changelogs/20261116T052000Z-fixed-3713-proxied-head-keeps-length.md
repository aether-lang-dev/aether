- **An answer to HEAD through the proxy keeps the upstream's
  Content-Length (#3713).** The proxy middleware set the empty body of a
  HEAD answer, which rewrote the upstream's Content-Length to 0, and a cache
  hit did the same; the server then added a 0 to a kept-alive HEAD answer
  that stated no length. A client asking a resource's size by HEAD read 0.
  The length is now the upstream's as it stated it, or none, on every path.
