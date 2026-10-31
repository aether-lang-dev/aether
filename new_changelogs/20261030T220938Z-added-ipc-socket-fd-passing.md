- **`std.ipc` gains a two-way socket channel that passes open descriptors.**
  `os.run_socket(prog, argv, env)` spawns a child with one end of an
  `AF_UNIX` stream pair at fd 3 (`AETHER_IPC_FD=3`, `AETHER_IPC_KIND=socket`),
  through the same spawn path as `os.run_pipe`, and `os.socketpair()` makes a
  pair within one process; both ends are close-on-exec. `std.ipc` adds
  `parent_channel_kind()` (`"pipe"`, `"socket"` or `""`), a binary-safe
  `read(ch, max)`, and `send_fd(ch, fd, payload)` / `recv_fd(ch, max)`.
  `recv_fd` accepts exactly one descriptor per message: one with none,
  several, truncated control data or another kind of control message is an
  error, and every descriptor that arrived is closed, so a hostile peer
  cannot fill the receiver's descriptor table. Received descriptors are
  close-on-exec (`MSG_CMSG_CLOEXEC`; `fcntl` straight after on macOS).
  `ipc.write` now writes every byte itself, continuing a short write and
  retrying an interrupted one, and a socket whose peer has gone is an error
  rather than SIGPIPE; it no longer goes through `std.net.fd_write`.
  `parent_channel()`, `write_close`, `os.run_pipe` and
  `os.run_pipe_drain_and_wait` are unchanged. Under `--emit=lib`, importing
  `std.ipc` now needs `--with=os`, as `std.os` does. The socket channel and
  descriptor passing are POSIX-only; on Windows they report that they are
  unsupported.
