- **A request head has a deadline from its first byte.** The only bound on
  reading a head was the per-read idle timeout, so a client that sent one
  byte just inside it, over and over, held a worker for as long as it liked,
  and a few such clients per worker held them all. A head must now arrive
  whole within 30 s of its first byte, or the server answers 408 and closes;
  `http.server_set_head_timeout` changes the bound and `0ns` turns it off.
  Deadlines for request bodies and for the TLS handshake remain open
  (#3737).
