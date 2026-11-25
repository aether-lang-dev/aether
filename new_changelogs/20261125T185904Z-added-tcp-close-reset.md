- **`tcp.close_reset(sock)` closes a connection with a reset on every
  platform.** Closing a socket with bytes unread resets the connection on
  Linux, macOS and Windows, but FreeBSD ends it with an orderly FIN, so a
  peer written to fail a connection the hard way could not do it there:
  the reverse proxy's reset test saw a bare close on FreeBSD and its 502 did
  not name the reset. `close_reset` sets `SO_LINGER` with a zero timeout and
  closes, so the peer's next read or write fails with "connection reset"
  wherever it runs.
