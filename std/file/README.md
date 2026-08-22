# std.file

Explicit file handles: open, read, write, close.

`std.fs` reads and writes whole files in one call, which is what most code
wants. `std.file` is for when you need the handle itself — a long-lived
descriptor, an incremental write, or an fd to hand to something else.

The example **compiles but is not run** in CI: it touches the filesystem.

```aether
import std.file

main() {
    handle, err = file.open("/tmp/notes.txt", "w")
    if err != "" {
        println("open failed: ${err}")
        return
    }

    werr = file.write(handle, "first line\n")
    if werr != "" {
        println("write failed: ${werr}")
    }

    file.file_close(handle)

    println("exists: ${file.file_exists("/tmp/notes.txt")}")
    file.delete("/tmp/notes.txt")
}
```

`fd(handle)` exposes the underlying descriptor, which is what a caller passes
to `std.tcp`'s poll or to a C interface expecting an `int`.

Modes follow C's `fopen`: `"r"`, `"w"`, `"a"`, with `"b"` where binary matters
(no-op on POSIX, load-bearing on Windows).

A handle must be closed. Unlike `std.fs`'s whole-file calls, nothing cleans up
for you at the end of the scope.

## Exports

`open`, `read`, `write`, `delete`, `size`, `fd`, `file_close`, `file_exists`.
