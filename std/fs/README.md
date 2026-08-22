# std.fs

Files and directories: read, write, copy, delete, and the metadata around
them.

Every operation returns an error string — `""` on success — rather than
throwing, so a caller decides what a missing file means. That matters more
here than elsewhere: "not found" is a normal outcome for a config lookup and a
fatal one for a required input, and only the caller knows which.

The example below **compiles but is not run** in CI: it touches the
filesystem, and a documentation example should not need a scratch directory to
be checked. Everything in it is type-checked against the real API.

```aether
import std.fs

main() {
    err = fs.write("/tmp/note.txt", "hello")
    if err != "" {
        println("write failed: ${err}")
        return
    }

    body, rerr = fs.read("/tmp/note.txt")
    if rerr != "" {
        println("read failed: ${rerr}")
        return
    }
    println(body)

    fs.delete("/tmp/note.txt")
}
```

`read` and `write` handle text. For binary payloads use `read_binary` and
`write_binary`, which carry an explicit length so a buffer with embedded NUL
bytes survives intact — the text forms would stop at the first zero.

`write_atomic` writes to a temporary file and renames it into place, so a
reader never observes a half-written file and a crash mid-write leaves the
old contents rather than a truncated one. Prefer it for anything a
concurrent process might read.

Errors carry a **kind** as well as a message: `last_os_error()` pairs with the
`KIND_*` constants, so a caller can branch on `KIND_NOT_FOUND` versus
`KIND_PERMISSION_DENIED` rather than matching on error text.

A hardcoded `/tmp` is fine on POSIX and wrong on Windows, where it may not
exist. Resolve `TEMP`, then `TMPDIR`, then `/tmp` — see #1702 for the
accessor that will make that one call.

## Exports

`read`, `write`, `write_binary`, `write_atomic`, `read_binary`, `pread`,
`pwrite`, `pread_into`, `copy`, `move`, `rename`, `delete`, `unlink`,
`exists`, `size`, `mtime`, `file_stat`, `realpath`, `chmod`, `ftruncate`,
`fsync`, `symlink`, `readlink`; `create_dir`, `create_dir_with_mode`,
`mkdir_p`, `delete_dir`, `list_dir`, `glob`, `glob_multi`, `walk`;
`watch_open`, `watch_wait`, `watch_close`; `statvfs`, `mounts`, `block_info`;
`last_os_error` with the `KIND_*` constants; and the path helpers `clean`,
`rel`, `join_clean`, `is_within_base`.
