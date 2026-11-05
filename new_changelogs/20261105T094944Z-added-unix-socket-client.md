- **Unix-domain sockets, and HTTP over them.** `tcp.connect_unix(path)` and
  `tcp.listen_unix(path)` return the handles `tcp.connect` and `tcp.listen`
  do, so every std.tcp read, write, poll and close works on them;
  `client.set_unix_socket(req, path)` sends a `std.http.client` request over
  a socket path instead of dialling the URL's host, which still names the
  Host header (curl's `--unix-socket`): the Docker and Podman Engine APIs at
  `/var/run/docker.sock` and Podman's machine socket. `client.response_take_conn(resp)`
  takes the connection out of a streaming response after a `101` or a
  hijacking `200`, as a std.tcp socket plus the bytes already read past the
  head, for Docker's `attach` and exec `start`. A path too long for
  `sockaddr_un` is refused, never truncated. In an enforce block a connect
  needs `fs_write` on the path (the kernel's rule for a Unix socket), asked
  on every HTTP request so a pooled connection does not skip it; a listen
  needs `tcp_listen` too. Linux, macOS, the BSDs, and Windows 10 1803 or
  later; an older Windows says it has no `AF_UNIX`. A write to a Unix socket
  the peer closed is EPIPE rather than a SIGPIPE that ended the process, and
  the HTTP client sends with `MSG_NOSIGNAL` on Linux.
