# std.fs

Files and directories: read, write, copy, delete, and the metadata around
them.

Every operation returns an error string (`""` on success) rather than
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

`read` and `write` carry every byte of a string, embedded NULs included.
`read_binary` and `write_binary` take or return an explicit length, for
callers that hold a raw buffer rather than a string.

Every entry point carries bytes verbatim on every platform, `open` included:
a handle from `fs.open(path, "w")` is binary on Windows too, so a `\n` is one
byte on disk and `pread` / `pwrite` through it are byte-exact (C's text-mode
`\r\n` translation never applies). A mode that spells `t` explicitly
(`"wt"`) asks for C's text mode and gets it.

`write_atomic` writes to a temporary file and renames it into place, so a
reader never observes a half-written file and a crash mid-write leaves the
old contents rather than a truncated one. Prefer it for anything a
concurrent process might read.

Errors carry a **kind** as well as a message: `last_os_error()` pairs with the
`KIND_*` constants, so a caller can branch on `KIND_NOT_FOUND` versus
`KIND_PERMISSION_DENIED` rather than matching on error text.

A hardcoded `/tmp` is fine on POSIX and wrong on Windows, where it may not
exist. Resolve `TEMP`, then `TMPDIR`, then `/tmp`: see #1702 for the
accessor that will make that one call.

## Private files on Windows

Windows keeps no mode bits, so `chmod` and `create_dir_with_mode` emulate
what a mode means there. The owner-write bit (0o200) clears or sets the
read-only attribute. A mode with no group or other bit (0o600, 0o700, 0o400)
gives the file or directory a protected DACL that inherits nothing from its
folder and admits only the current user, SYSTEM and the Administrators group
(OpenSSH for Windows' rule for a private key), so another user cannot open
it; a directory passes that on to what is created in it. A mode with a group
or other bit (0o644) puts back what the folder grants. On a volume without
ACLs (FAT, exFAT) an owner-only mode fails with `KIND_IO` instead of leaving
the file readable.

`copy` carries that over as it carries mode bits on POSIX: a copy of an
owner-only file (one with a protected DACL) is owner-only, made so before
its bytes go in, whether it is a new file or overwrites one; a copy of a
file that inherits its folder's access takes the destination folder's
access, as `chmod 0o644` would. When the destination cannot be made
owner-only, nothing is copied and the call fails (`KIND_IO`, or
`KIND_PERMISSION_DENIED`).

## Replacing a file other processes also edit

`stat(path, follow)` returns a `Stat` value with what `file_stat` leaves out:
the permission bits, the inode and device, the link count and a nanosecond
mtime. Two writes of the same size in the same second differ in `mtime_ns`,
and a file replaced by a rename has a new `ino`, so `(ino, size, mtime_ns)`
tells whether a file changed since it was read. `follow` 0 describes a
symlink itself.

The descriptor calls work on the `int` std.io's `fd_*` calls take:
`lock_open` and `try_lock` / `unlock` hold an advisory lock (flock(2),
LockFileEx on Windows) that is polled rather than waited on, so a wait stays
cancelable; `create_exclusive(path, mode)` creates with `O_EXCL`, the safe
first step of an atomic replacement; `fchmod`, `fd_fsync`, `fd_truncate`,
`fd_size` and `fd_pwrite_all` finish it. Write with `io.fd_write`, close
with `io.fd_close`. They return `(value, KIND_*, message)`, as `copy` and
`chmod` do.

```aether
import std.fs
import std.io
import std.string

// Replace `path` with `text`, keeping its mode.
replace(path: string, text: string) -> string {
    old, kind, err = fs.stat(path, 1)
    if kind != fs.KIND_OK { return err }
    tmp = "${path}.new"
    fd, _, cerr = fs.create_exclusive(tmp, 0o600)
    if fd < 0 { return cerr }
    if io.fd_write(fd, string.bytes(text)) != 0 { io.fd_close(fd); return "write failed" }
    fs.fchmod(fd, old.mode)
    fs.fd_fsync(fd)
    io.fd_close(fd)
    return fs.rename(tmp, path)
}

main() {
    println(replace("/tmp/note.txt", "new text"))
}
```

## Exports

`read`, `write`, `write_binary`, `write_atomic`, `read_binary`, `pread`,
`pwrite`, `pread_into`, `copy`, `move`, `rename`, `delete`, `unlink`,
`exists`, `size`, `mtime`, `file_stat`, `stat` (and `Stat`), `realpath`, `chmod`, `ftruncate`,
`fsync`, `symlink`, `hard_link`, `readlink`; `create_dir`, `create_dir_with_mode`,
`mkdir_p`, `delete_dir`, `list_dir`, `glob`, `glob_multi`, `walk`;
`watch_open`, `watch_wait`, `watch_close`; `statvfs`, `mounts`, `block_info`;
`lock_open`, `try_lock`, `unlock`, `create_exclusive`, `fchmod`, `fd_fsync`,
`fd_truncate`, `fd_size`, `fd_pwrite_all`;
`last_os_error` with the `KIND_*` constants; and the path helpers `clean`,
`rel`, `join_clean`, `is_within_base`.
