- **An https request through a forward proxy reads the proxy's CONNECT reply
  strictly, and keeps the bytes after it for the tunnel (#3557).** The reply
  was read with one `recv` into 1024 bytes and everything past its head was
  dropped, so bytes a proxy sent behind its 200 never reached the TLS
  handshake; and its status was read with `atoi`, which took `2000` and `20`,
  and a reply that was not HTTP at all, without a word. The client now takes
  the head and nothing more (the rest stays in the socket for the
  handshake), and refuses a reply whose status line is not `HTTP/1.` with a
  three-digit status, whose head holds a NUL or a bare CR, or whose head runs
  past 4096 bytes, with "proxy CONNECT: malformed reply from the proxy" or
  saying which.
