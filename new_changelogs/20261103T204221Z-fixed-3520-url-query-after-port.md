- **A URL with a query or a fragment right after its port works again
  (#3520).** The HTTP client ended a URL's authority only at a `/`, so the
  strict port reader took `8080?x=1` for the port and refused
  `http://127.0.0.1:8080?x=1` and `http://127.0.0.1:8080#f` as malformed,
  where before it sent `GET /` and dropped the query. The authority now ends
  at the first `/`, `?` or `#` (RFC 3986), the request target keeps the
  query, with an empty path sent as `/`, and the fragment is never sent,
  which it was after a path (`/p#f` went out as is). A forward proxy URL
  and a `ws://` URL are split the same way.
