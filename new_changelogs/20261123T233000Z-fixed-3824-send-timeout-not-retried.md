- **A POST whose write timed out is not written again, on Windows too.**
  A write that waits out the socket's send timeout is never written again,
  since the upstream is alive and Winsock leaves what a timed-out send sent
  undefined. Winsock says so with WSAETIMEDOUT, where POSIX says EAGAIN, and
  the HTTP client read it as an I/O error: a POST whose head timed out
  before a byte of it counted as sent went again on a fresh connection. A
  failed write is now classified as a failed read is, one rule for both, and
  a timeout is a timeout on every platform. A failed TLS write is classified
  by the TLS layer's own answer, not by an errno it did not set: a write on
  a connection a protocol error ended left a stale EAGAIN behind, and the
  proxy's event loop waited for the socket to turn writable until the
  deadline (#3824).
