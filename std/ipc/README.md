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

`os.run_stdio` makes a third, separate channel: the child's own stdin and
stdout (see [The stdio channel](#the-stdio-channel)). It is not a
`parent_channel()`.

`parent_channel()` returns the inherited descriptor, or `-1` when there is
none; not being spawned with one is a normal outcome, not an error.
`parent_channel_kind()` says which kind it is: `"pipe"`, `"socket"`,
`"stdio"` in a child of `os.run_stdio` (whose `parent_channel()` is `-1`:
its channel is stdin and stdout), or `""` when there is none.

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
  (below); a frame timeout says whether the stream is still in step.

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
bounds the whole frame, header and payload together. `read_frame` is the
same call with no timeout.

A timeout is one of two errors, and they mean different things for the
stream:

- `ipc.TIMED_OUT_BETWEEN_FRAMES`: the deadline passed before the first byte
  of a frame arrived. Nothing was consumed, the stream is between frames,
  and it is fit to use: write to the peer (a CANCEL, say) and read again.
- `ipc.TIMED_OUT_INSIDE_FRAME`: the deadline passed part-way through a
  header or payload. The bytes already read are gone with the call, so the
  stream is out of step: close the channel.

Any other error leaves the stream unusable too. So a host needs no
`wait_readable` before each frame to know whether it can still talk to a
worker that has gone quiet. The reader keeps no partial frame between calls
on purpose: that would be per-descriptor state for the runtime to track and
free, and a host that wrote to the peer meanwhile would still be talking
across a half-read frame.

```aether,fragment
import std.ipc

// Take one reply from a worker, however it misbehaves: at most 64 KiB, in
// at most two seconds. A worker that has said nothing yet is asked to stop
// and gets one more second; one that stalled inside a frame is cut off.
answer_from(ch: int) -> string {
    data, err = ipc.read_frame_timeout(ch, 65536, 2000)
    if err == ipc.TIMED_OUT_BETWEEN_FRAMES {
        ipc.write_frame(ch, "CANCEL")
        data, err = ipc.read_frame_timeout(ch, 65536, 1000)
    }
    if err != "" { return "" }
    return data
}
```

## The stdio channel

`os.run_stdio(prog, argv, env)` spawns the child with its stdin and stdout
each a pipe the parent keeps (stderr is inherited) and returns
`(to_child_fd, from_child_fd, pid, err)`. Both descriptors are
close-on-exec; `pid` is reaped with `os.wait_pid` (or bounded with
`os.wait_pid_timeout`, ended with `os.kill`). The child's environment
carries `AETHER_IPC_KIND=stdio` and no `AETHER_IPC_FD`. The other end can be
a program in any language, or a container runtime that passes its stdin and
stdout through (`wslc run --rm -i`, `docker run -i`).

The frames above work over the pair as they do over a socket, one
descriptor for each direction: `write_frame(to_child_fd, ...)`,
`read_frame(from_child_fd, max)`, `read_frame_timeout` and `wait_readable`
on `from_child_fd`. There is no descriptor passing over this channel: the
pipes carry bytes only (use `os.run_socket` for that).

With the channel on stdout, anything else the child prints there lands in
the middle of the frames, and anything else that reads stdin takes frame
bytes. An Aether child therefore claims the pair first thing in `main`
with `stdio_channel()`, which returns two fresh descriptors for the channel
and then points fd 0 at the null device and fd 1 at stderr:

```aether,fragment
import std.ipc

// The child: answer each request frame, until the parent closes its end.
main() {
    inp, out, err = ipc.stdio_channel()
    if err != "" {
        ipc.write(2, "${err}\n")
        exit(1)
    }
    println("started")   // goes to stderr, not into a frame
    while true {
        req, rerr = ipc.read_frame(inp, 65536)
        if rerr != "" { return }
        ipc.write_frame(out, "ok ${req}")
    }
}
```

`stdio_channel()` returns `(in_fd, out_fd, "")`, or `(-1, -1, err)` with
nothing changed. It flushes stdout, moves fd 0 and fd 1 to two new
close-on-exec descriptors (non-inheritable handles on Windows), points fd 0
at `/dev/null` (`NUL` on Windows), so a read of stdin gets end of file at
once, and fd 1 at stderr (the null device when there is none), so a later
`print`, a library diagnostic or a panic message goes to stderr. On Windows
the Win32 standard handles are pointed the same way (`SetStdHandle`), so
code that writes through `GetStdHandle` follows too. It clears
`AETHER_IPC_KIND`, so this process's own children do not think they are on
the channel. A second call fails with `"ipc.stdio_channel: already
claimed"`. It does not need `AETHER_IPC_KIND`: a container runtime does not
pass it through, and the claim works the same.

What it cannot mask:

- output written to stdout **before** the call: it is flushed out ahead of
  the first frame, where the parent reads it as a (bad) frame header. A
  child prints nothing before it claims;
- bytes C stdio read ahead from stdin before the call: they are lost;
- code holding its own copy of fd 1 or of the old stdout handle (a
  descriptor dup'd before the call, a child process started before it, a
  library that cached the handle), which still writes into the channel.

Closing `to_child_fd` with `io.fd_close` gives the child end of stream on
its channel; `from_child_fd` reads end of stream once the child (and
whatever it passed its stdout to) has exited. Writing to a child that has
gone is an error, not SIGPIPE.

It works on Windows too (anonymous pipes, handed back as binary CRT
descriptors), which is why a host that has to run there uses it.

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

- **Linux, FreeBSD, macOS:** both kinds of channel, the stdio channel, and
  descriptor passing. A write to a pipe whose reader has gone is `EPIPE`,
  not SIGPIPE: std.ipc holds SIGPIPE off the writing thread for the write.
- **Windows:** no parent channel (`parent_channel()` is `-1`); `write`,
  `read`, `write_frame` and `read_frame` are plain CRT descriptor I/O, with
  the descriptor put in binary mode (a child's fd 0 is in text mode by
  default, which would turn CR LF into LF and stop at a `0x1a` byte).
  `os.run_stdio` and `stdio_channel()` work, and `wait_readable` and
  `read_frame_timeout` work on anonymous pipes such as theirs: a CRT
  descriptor has no `poll`, so they ask the pipe how many bytes wait
  (`PeekNamedPipe`), sleeping 1 to 8 ms between asks.
  `Sleep` rounds up to the system timer tick (about 15.6 ms by default), so
  bytes and deadlines are noticed within about a tick; a timeout of 0 asks
  once and does not sleep. A writer that has closed reads as ready, and the
  read then gives end of stream. On any descriptor other than a pipe they
  return a `not a pipe` error rather than guess. `os.run_socket`,
  `os.socketpair`, `send_fd`, `recv_fd` and `recv` report that they are
  unsupported; Windows passes handles with `DuplicateHandle`, a different
  shape.
- **Builds without a filesystem** (WASM, embedded): every function
  exists and reports that it is unavailable.

Under `--emit=lib`, importing `std.ipc` needs `--with=os`, as `std.os`
does: passing descriptors hands out authority.

## Exports

`parent_channel`, `parent_channel_kind`, `write`, `write_close`, `read`,
`send_fd`, `recv_fd`, `recv`, `write_frame`, `read_frame`,
`read_frame_timeout`, `wait_readable`, and the constants
`TIMED_OUT_BETWEEN_FRAMES` and `TIMED_OUT_INSIDE_FRAME`.
