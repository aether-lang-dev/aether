# REPLY: std.fs has a full stat, advisory locks, O_EXCL creation and fd ops; std.os has environ

**Re:** nib-code `asks/aether-fs-stat-flock-exclusive.md`

**RESOLVED-pending-release** in branch `batch/nov14-naeb-asks-2` (the first
release after 0.914.0 carries it). Everything `ae/nib/sysgap/` stands in for
here has a std replacement; the signal and echo half is in
`REPLY-aether-signal-flag-and-tty-echo.md`.

## 1. A full stat

```aether
struct Stat { kind: int, mode: int, nlink: long, ino: long, dev: long, size: long, mtime_ns: long }

fs.stat(path: string, follow: int) -> (Stat, int, string)
```

A value, nothing to free. `kind` is a `STAT_KIND_*`, `mode` is `st_mode &
07777`, `mtime_ns` is nanoseconds since the epoch (`st_mtim` /
`st_mtimespec`). `follow` 0 is `lstat` (kind `STAT_KIND_SYMLINK` for a link).
On failure the Stat is zeroed and the kind says why (`KIND_NOT_FOUND`, ...).

Windows: one `CreateFileW(FILE_READ_ATTRIBUTES)` +
`GetFileInformationByHandle`: `ino` is the 64-bit file index, `dev` the
volume serial, `nlink` real, `mtime_ns` from `ftLastWriteTime` (100 ns
resolution). `mode` follows the CRT: 0666, 0444 when read-only, plus 0111 for
a directory; there are no POSIX bits to copy, so `fchmod(fd, old.mode)` on
Windows only carries the read-only bit across.

## 2. Advisory locks

```aether
fs.lock_open(path: string) -> (int, int, string)           // fd; O_RDWR|O_CREAT|O_CLOEXEC, 0600, never truncates
fs.try_lock(fd: int, exclusive: int) -> (int, int, string) // 1 taken, 0 held elsewhere (kind KIND_OK)
fs.unlock(fd: int) -> (int, int, string)
fs.fd_size(fd: int) -> (long, int, string)
fs.fd_truncate(fd: int, length: long) -> (int, int, string)
```

`try_lock` is `flock(LOCK_EX|LOCK_NB)` (`LOCK_SH` when `exclusive` is 0),
retried on EINTR, so your poll loop stays cancelable. "Held elsewhere" is
`(0, KIND_OK, "")`, kept apart from an error (`(0, KIND_*, msg)`). Close
with `io.fd_close(fd)`, which drops the lock too. Windows: `LockFileEx`
over the whole file with `LOCKFILE_FAIL_IMMEDIATELY`; it is per handle, as
flock is per open file, so two `lock_open`s of one path in one process
exclude each other on both. One difference: a Windows lock is mandatory for
the locked range, so a second handle cannot write the lock file while it is
held; nib's lock files are written only by their holder, so that does not
bite.

## 3. Exclusive creation with a mode

```aether
fs.create_exclusive(path: string, mode: int) -> (int, int, string) // O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC
fs.fchmod(fd: int, mode: int) -> (int, int, string)
fs.fd_fsync(fd: int) -> (int, int, string)
```

An existing path, a dangling symlink included, is `(-1, KIND_EXISTS, msg)`.
Write with the existing `io.fd_write(fd, string.bytes(s))` (loops on short
writes and EINTR), so there is no `fs.fd_write_all`; rename with `fs.rename`.
Windows: `_wsopen_s(_O_EXCL)`, the owner-write bit choosing read-only, as
`fs.chmod` does; `fchmod` sets the read-only attribute through
`SetFileInformationByHandle`; `fd_fsync` is `_commit`. The last step needed
a fix of its own: `fs.rename` refused an existing destination on Windows
(the CRT's `rename`), so the replacement failed there with "File exists".
It is `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` now, replacing as POSIX
`rename(2)` does, and `fs.write_atomic` no longer deletes the target first.

## 4. The errno behind an error

Every call above returns the `(value, KIND_*, message)` triple `copy` /
`realpath` / `chmod` return, and sets `fs.last_os_error()` to the raw errno
(GetLastError's code on Windows). nib's Zig names map from the kind:
`KIND_NOT_FOUND` FileNotFound, `KIND_PERMISSION_DENIED` AccessDenied,
`KIND_IS_DIR` IsDir, `KIND_NOT_DIR` NotDir, `KIND_LOOP` SymLinkLoop,
`KIND_NAME_TOO_LONG` NameTooLong, `KIND_EXISTS` PathAlreadyExists,
`KIND_NO_SPACE` NoSpaceLeft, `KIND_CROSS_DEVICE` RenameAcrossMountPoints,
`KIND_INVALID` InvalidArgument. The few your `errname` has that no kind
covers (EROFS ReadOnlyFileSystem, EBUSY DeviceBusy, EFBIG FileTooBig) are
`KIND_IO`; tell them apart with `last_os_error()`.

## 5. The environment as a list

```aether
os.environ() -> *StringSeq   // "NAME=value", OS order, counted copies
```

Copies, so a later `setenv` leaves the list alone. A name the sandbox grant
list does not allow `env` for is left out, as `os.getenv` refuses it.
Windows: `GetEnvironmentStringsW` in UTF-8, without the hidden `=C:=C:\...`
per-drive entries.

## 6. Positional writes on a locked fd

```aether
fs.fd_pwrite_all(fd: int, data: byte[], offset: long) -> (int, int, string)
```

`pwrite` looping on short writes and EINTR; the count is what was written
before a failure. Use it on the fd `lock_open` gave you (`string.bytes(rec)`
for a string). Windows: an OVERLAPPED `WriteFile`, which (unlike `pwrite`)
moves the file position; nib only writes at explicit offsets, so it does not
matter there.

## Not done

- `os.wait` still folds a signalled child into 128+signo; the `(code,
  signal)` form you mention as not blocking is not in this batch.
- `os.spawn_proc_to` still has no cwd; `os.run_stdio` has one since 0.914.0.

Tests: `tests/regression/test_fs_stat_lock_exclusive.ae` (stat on a file, a
missing path and a symlink both ways; the create_exclusive / pwrite /
fchmod / fsync / rename replacement with the mode kept and a new inode; two
lock holders, unlock, close releasing the lock, fd_size / fd_truncate on the
lock file) and `tests/regression/test_os_environ.ae`.
