- **The reverse proxy's retry policy retries TRACE.** Which methods may be
  sent again was decided twice: the HTTP client's rule, which follows RFC
  9110 9.2.2, and a list of the proxy's own that left TRACE out. The proxy
  now reads the client's rule, so `opts_set_retry_policy` retries GET,
  HEAD, OPTIONS, TRACE, PUT and DELETE, and still never POST or PATCH
  (#3824).
