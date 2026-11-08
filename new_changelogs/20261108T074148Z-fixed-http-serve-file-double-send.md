- **`http.serve_file` no longer sends a corrupt response where its sendfile(2)
  drain fails or does not exist.** The fast path sent the headers, then on a
  drain that came up short fell back to the buffered send and serialized the
  whole response again: a second status line and header block where the client
  was reading the body, and no body behind it. On FreeBSD (and any platform
  other than Linux and macOS) the drain was not implemented, so every file
  served that way was corrupt (`curl: (18) ... bytes missing`, or "Excess found
  writing body"); on Linux and macOS it happened when a client stopped reading
  past the send timeout. Once the headers are out the response is committed:
  a failed drain now closes the connection and sends nothing more. FreeBSD now
  has a real sendfile(2) drain, and platforms without one read the file into
  the body instead of taking the fast path. Tests:
  `tests/integration/http_sendfile` checks the keep-alive requests share one
  connection, and `tests/integration/http_sendfile_cut` covers the failed
  drain.
