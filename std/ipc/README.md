# std.ipc

Inter-process communication between parent and child processes.

`std.ipc` provides child-side discovery and bidirectional communication over
channels inherited from `std.os` spawns, as well as file descriptor passing over
stream sockets on Linux, FreeBSD, and macOS.

## Channel Kinds

A process spawned by `std.os` may inherit one of two channel kinds:

1. **`"pipe"`**: A one-way report channel created by `os.run_pipe` or
   `os.run_pipe_drain_and_wait`. The child writes into this pipe to send
   progress or structured reports without colliding with stdout/stderr.
2. **`"socket"`**: A two-way Unix domain stream socket pair created by
   `os.run_socket`. Both parent and child can send and receive binary data and
   pass open file descriptors across the boundary.

`parent_channel()` returns the inherited channel descriptor (negative when none
exists), preserving backward compatibility. `parent_channel_kind()` returns
`"pipe"`, `"socket"`, or `""` if no channel was inherited.

## Functions

- `parent_channel() -> int`: Returns the inherited channel descriptor, or `-1` if none.
- `parent_channel_kind() -> string`: Returns `"pipe"`, `"socket"`, or `""`.
- `write(ch: int, data: string) -> string`: Write bytes to channel `ch`. Returns `""` on success or an error message.
- `write_close(ch: int, data: string) -> string`: Write bytes to channel `ch` and close the descriptor.
- `read(ch: int, max: int) -> (string, string)`: Binary-safe read up to `max` bytes. Returns `(data, err)`. On EOF, returns `("", "")`.
- `send_fd(ch: int, fd: int, payload: string) -> string`: Sends one open file descriptor with attached payload (at least 1 byte). Returns `""` on success or an error message.
- `recv_fd(ch: int, max: int) -> (int, string, string)`: Receives one open file descriptor and payload up to `max` bytes. Returns `(fd, payload, err)`.

## Safety Guarantees

When passing and receiving file descriptors via `send_fd` and `recv_fd`:

1. **Exactly one descriptor per message:** If control data is truncated (`MSG_CTRUNC`), or more than one descriptor is received, or an unexpected control message arrives, all received descriptors are closed immediately and an error is returned to prevent descriptor leakage.
2. **Close-on-exec at receipt:** Received descriptors have `FD_CLOEXEC` applied atomically upon receipt (`MSG_CMSG_CLOEXEC` on Linux and FreeBSD; immediate `fcntl` fallback on macOS).
3. **No SIGPIPE:** Socket operations use `MSG_NOSIGNAL` (Linux, FreeBSD) and `SO_NOSIGPIPE` (macOS). Writing to a closed peer returns an error instead of terminating the process with `SIGPIPE`.
4. **Partial writes and EINTR:** Writes and sends loop on partial writes and retry on `EINTR`. Success is never reported on a truncated write.
5. **No silent missing descriptors:** A message without ancillary control data arriving at `recv_fd` produces an explicit error, returning `fd = -1`.
6. **Descriptive error reasons:** Errors detail the failure cause and include OS `strerror(errno)` where appropriate.

## Platform Support

- **Linux, FreeBSD, macOS:** Full support for pipe channels, socket channels, and file descriptor passing.
- **Windows:** Pipe back-channel and socket/fd passing APIs return clear "unsupported on Windows" errors.
- **WASM / Minimal builds:** Stubs returning "unavailable" link cleanly in all environments.

## Exports

`parent_channel`, `parent_channel_kind`, `write`, `write_close`, `read`, `send_fd`, `recv_fd`.
