# std.ipc

A channel to the parent process: a report pipe, or a two-way socket that
can also pass open file descriptors.

When a program is spawned by `std.os`, it may inherit an extra descriptor
that its parent holds the other end of. `std.ipc` finds it and reads,
writes and passes descriptors over it, without colliding with stdout,
which the parent may be capturing for other reasons.

## Two kinds of channel

| Kind | Parent spawns with | Direction | Descriptor passing |
|---|---|---|---|
| `"pipe"` | `os.run_pipe`, `os.run_pipe_drain_and_wait` | child to parent | no |
| `"socket"` | `os.run_socket` | both ways | yes |

`parent_channel()` returns the inherited descriptor, or `-1` when there is
none; not being spawned with one is a normal outcome, not an error.
`parent_channel_kind()` says which kind it is: `"pipe"`, `"socket"`, or
`""` when there is none.

The example **compiles but is not run** in CI: it needs a parent that opened
the channel, and outside one `parent_channel` reports that there is none.

```aether
import std.ipc

main() {
    ch = ipc.parent_channel()
    if ch < 0 {
        println("no parent channel")
        return
    }

    werr = ipc.write(ch, "progress: 50%\n")
    if werr != "" {
        println("write failed: ${werr}")
    }

    // write_close sends a final payload and closes, which is what
    // signals the parent that no more is coming.
    ipc.write_close(ch, "done\n")
}
```

`write_close(fd, bytes)` writes a last payload and closes in one call. That
close matters: a parent draining the channel blocks until it sees EOF, so a
child that exits without closing leaves the parent waiting until it notices
the process is gone.

## The socket channel

`os.run_socket(prog, argv, env)` spawns the child with one end of an
`AF_UNIX` stream socket pair at fd 3 (`AETHER_IPC_FD=3`,
`AETHER_IPC_KIND=socket`) and returns `(fd, pid, err)`: the parent's end,
close-on-exec, and the child to reap with `os.wait_pid`.
`os.socketpair()` makes a connected pair within one process.

On either end:

- `write(ch, data) -> err` writes every byte, or says why it could not.
- `read(ch, max) -> (data, err)` returns what one read brings, up to
  `max` bytes: possibly less than was written, since the channel is a byte
  stream and framing is the caller's. `("", "")` is end of stream.
- `send_fd(ch, fd, payload) -> err` passes the open descriptor `fd`,
  attached to `payload`, which must be at least one byte. The receiver gets
  its own descriptor for the same open file; the sender's stays open.
- `recv_fd(ch, max) -> (fd, payload, err)` receives one descriptor and the
  bytes that came with it.

A parent that has checked a request can open a file and hand the child the
open descriptor, so a child that cannot open files itself still reads it:

```aether
import std.ipc
import std.io
import std.os
import std.list

main() {
    argv = list.new()
    ch, child, err = os.run_socket("./worker", argv, null)
    list.free(argv)
    if err != "" {
        println("spawn failed: ${err}")
        return
    }
    f, ferr = io.fd_open_read("data.txt")
    if ferr == "" {
        ipc.send_fd(ch, f, "data.txt")
        io.fd_close(f)
    }
    report, _ = ipc.read(ch, 4096)
    println(report)
    io.fd_close(ch)
    os.wait_pid(child)
}
```

## What the receiver can rely on

1. **Exactly one descriptor per message.** A message whose control data
   was truncated, that carries more than one descriptor, or a control
   message other than `SCM_RIGHTS` is an error, and every descriptor that
   did arrive is closed first, so a hostile peer cannot fill the
   receiver's descriptor table with messages it refuses. `recv_fd` makes
   room for more descriptors than one message can carry (Linux allows 253,
   macOS 254), so none arrives unseen: macOS installs every descriptor of a
   truncated message, including those past the buffer.
2. **Close-on-exec at receipt.** On Linux and FreeBSD the descriptor is
   created close-on-exec (`MSG_CMSG_CLOEXEC`). macOS lacks that and sets
   it straight after receipt, so a `fork` on another thread in between can
   copy the descriptor into that child.
3. **No SIGPIPE.** Writing to a socket whose peer has gone is an error,
   not the end of the process (`MSG_NOSIGNAL`; `SO_NOSIGPIPE` on macOS,
   set on both ends `std.os` makes).
4. **Whole writes.** Short writes continue and interrupted ones retry;
   success means every byte went.
5. **No silent missing descriptor.** A message without one is an error
   saying so, never a `-1` alongside an empty error.
6. **Reasons.** Errors name the call and the cause, with the system's
   message where there is one.

## Platforms

- **Linux, FreeBSD, macOS:** both kinds of channel and descriptor passing.
- **Windows:** no parent channel (`parent_channel()` is `-1`); `write` and
  `read` are plain descriptor I/O; `os.run_socket`, `os.socketpair`,
  `send_fd` and `recv_fd` report that they are unsupported. Windows passes
  handles with `DuplicateHandle`, a different shape.
- **Builds without a filesystem** (WASM, embedded): every function
  exists and reports that it is unavailable.

Under `--emit=lib`, importing `std.ipc` needs `--with=os`, as `std.os`
does: passing descriptors hands out authority.

## Exports

`parent_channel`, `parent_channel_kind`, `write`, `write_close`, `read`,
`send_fd`, `recv_fd`.
