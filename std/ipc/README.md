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
  bytes that came with it; a message with none is an error.
- `recv(ch, max) -> (data, fd, err)` is the one receive: the bytes and the
  descriptor, if any, from a single `recvmsg`. `fd` is `-1` when the message
  carried none, so a peer need not announce a descriptor before sending it.
  `("", -1, "")` is end of stream.
- `write_frame(ch, data) -> err` and `read_frame(ch, max) -> (data, err)`
  carry whole messages (below).
- `wait_readable(ch, timeout_ms) -> (ready, err)` and
  `read_frame_timeout(ch, max, timeout_ms) -> (data, err)` bound a wait
  (below).

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

## Frames

The channel is a byte stream, so a reader that wants messages needs
framing, and one that reads a hostile peer needs a bound. A frame is

    +--------------------+------------------------+
    | length: 4 bytes,   | payload: `length`      |
    | big-endian, unsigned| bytes, any bytes      |
    +--------------------+------------------------+

`write_frame(ch, data)` writes the header and the payload in one write.
The payload may hold any byte: `0x00` and `0x0a` included, since the length
is the string's, not a terminator's. An empty payload is a frame of length 0;
4 GiB or more cannot be announced and is an error.

`read_frame(ch, max)` returns one whole payload and consumes exactly that
frame, never a byte of the next, so it keeps no state between calls and
a frame split at any offset across writes reads the same. `max` is a hard
cap checked on the header: a frame announcing more is an error, and not a
byte is allocated for it or read past the header. Choose `max` for what the
peer may reasonably send, not for what it claims.

A frame of length 0 reads `("", "")`. End of stream is never that: before a
header it is the error `end of stream`, inside a header or payload
`stream ended inside a frame`. Frames are read with plain reads, so a
descriptor riding on the bytes is dropped by the kernel; use `recv` for
descriptors.

## Bounding a wait

`wait_readable(ch, timeout_ms)` is `poll`: `(1, "")` when a read will not
block (bytes are there, or the peer has closed and the read gives end of
stream), `(0, "")` on a timeout. A negative timeout waits forever, `0` asks
without waiting. It bounds the first byte of any read.

A peer that sends a header and then stalls, or drips a byte at a time, gets
past a wait for the first byte, so `read_frame_timeout(ch, max, timeout_ms)`
bounds the whole frame, header and payload together, and says `timed out`.
After a timeout the stream is mid-frame: close the channel. `read_frame` is
the same call with no timeout.

```aether,fragment
import std.ipc

// Take one reply from a worker, however it misbehaves: at most 64 KiB, in
// at most two seconds.
answer_from(ch: int) -> string {
    data, err = ipc.read_frame_timeout(ch, 65536, 2000)
    if err != "" { return "" }
    return data
}
```

## Descriptors only go down

A descriptor travels from the parent to a child, never the other way. The
parent end of a channel `os.run_socket` made (the descriptor it returned)
refuses any descriptor its child sends, with no opt-in: `recv` and
`recv_fd` close it, and every other that rode on the same message, and return
an error, dropping the bytes that came with them. The child end, and both
ends of `os.socketpair`, accept.

The parent ends are recorded by the runtime when `os.run_socket` hands them
out, in a table in `runtime/utils/aether_spawn.h`
(`aether_ipc_mark_parent_end`), and forgotten when `io.fd_close` closes the
number, so a socket that later reuses it is not taken for one. Close a
channel with `io.fd_close` (`write_close` does). `IPC_MAX_FDS` stays what
bounds how many descriptors one message is looked at for.

## What the receiver can rely on

1. **At most one descriptor per message.** A message whose control data
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
- **Windows:** no parent channel (`parent_channel()` is `-1`); `write`,
  `read`, `write_frame` and `read_frame` are plain descriptor I/O;
  `os.run_socket`, `os.socketpair`, `send_fd`, `recv_fd` and `recv` report
  that they are unsupported, as do `wait_readable` and `read_frame_timeout`
  with a timeout (a CRT descriptor has no `poll`). Windows passes
  handles with `DuplicateHandle`, a different shape.
- **Builds without a filesystem** (WASM, embedded): every function
  exists and reports that it is unavailable.

Under `--emit=lib`, importing `std.ipc` needs `--with=os`, as `std.os`
does: passing descriptors hands out authority.

## Exports

`parent_channel`, `parent_channel_kind`, `write`, `write_close`, `read`,
`send_fd`, `recv_fd`, `recv`, `write_frame`, `read_frame`,
`read_frame_timeout`, `wait_readable`.
