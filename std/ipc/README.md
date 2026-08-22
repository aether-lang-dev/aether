# std.ipc

A back-channel to the parent process.

When a program is spawned by `std.os`'s `run_pipe`, it inherits an extra
descriptor that its parent reads. `std.ipc` is the child's end: a structured
way to report progress or results that does not collide with stdout, which the
parent may be capturing for other reasons.

The example **compiles but is not run** in CI: it needs a parent that opened
the channel, and outside one `parent_channel` reports that there is none.

```aether
import std.ipc

main() {
    // A bare fd, or negative when there is no channel — not being
    // spawned with one is a normal outcome, not an error.
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

The channel is **write-only from the child**: it is a report path, not a
conversation. For bidirectional work, spawn with a socket pair or use actors.

`write_close(fd, bytes)` writes a last payload and closes in one call. That
close matters: a parent draining the channel blocks until it sees EOF, so a
child that exits without closing leaves the parent waiting until it notices
the process is gone.

On Windows the parent-side pipe is not implemented (`std.os.run_pipe` is
POSIX-only), so a child there always reports no channel.

## Exports

`parent_channel`, `write`, `write_close`.
