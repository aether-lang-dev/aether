- **A POST the upstream already read is not sent to it again.** When a
  pooled upstream connection ended with no byte of an answer, the HTTP
  client and the reverse proxy (worker path and event loop alike) sent the
  request again on a fresh connection whatever its method, so an upstream
  that read a POST whole and then closed had it twice (RFC 9110 9.2.2). A
  failure at the read is now retried only for GET, HEAD, OPTIONS, TRACE,
  PUT and DELETE; another method gets the error, which the proxy answers
  502. A failed write is still retried for any method. So that an idle
  connection the upstream closed long ago does not cost a POST its answer,
  a pooled connection is checked before a non-idempotent request is written
  on it (a poll and a one-byte peek) and dropped for a fresh dial when the
  upstream has closed it, as Go's transport does (#3824).
