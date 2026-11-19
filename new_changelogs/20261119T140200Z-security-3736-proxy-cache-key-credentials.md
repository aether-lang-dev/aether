- **The proxy cache keys by the query and the Host, and keeps no
  credentialed answer.** The cache key was the path alone, so `/x?a=1` and
  `/x?a=2` shared an entry and one client could be served another's
  response. The key now carries the query as it is sent upstream, and the
  client's `Host` under `preserve_host`. A response to a request carrying
  `Authorization` was stored and replayed to every later client; it is now
  stored only when the response says `public`, `s-maxage` or
  `must-revalidate` (RFC 9111 3.5). A response carrying `Set-Cookie` is no
  longer stored, so its cookie is never replayed (#3736).
