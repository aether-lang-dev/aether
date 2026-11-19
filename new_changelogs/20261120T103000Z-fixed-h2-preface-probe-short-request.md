- **A short HTTP/1.0 request is answered on a server with HTTP/2 on.** The
  server read a new connection's first 24 bytes, the length of the HTTP/2
  preface, before deciding whether it spoke HTTP/2, so a whole request
  shorter than that (`GET / HTTP/1.0\r\n\r\n` is 18 bytes) waited for
  bytes its client would never send and was answered 408. The first byte
  that differs from the preface now settles it.
