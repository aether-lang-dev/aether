- **A failed pure-TLS https request says why, and no longer closes a socket
  twice.** Every handshake failure on the pure-Aether path reached the caller
  as one guessed sentence ("...most often means no CA bundle was found"),
  whether the chain was untrusted, the certificate was for another host, the
  server did not speak TLS 1.3 or the store was missing. The error is now
  `TLS handshake failed (pure-Aether TLS 1.3): <cause>`, with the cause
  `tls13_client` found. A server that answers the TLS-1.3-only ClientHello with
  a plaintext alert is reported as such (`server does not support TLS 1.3 (it
  answered with alert protocol_version)`) instead of as "server hello:
  truncated header", and a TLS 1.2 ServerHello says the client speaks TLS 1.3
  only. A failed pure handshake also closed the request's socket descriptor a
  second time after `tls13_client` had already closed it, which in a threaded
  program could close a descriptor another thread had just been given.
