- **`fs.stat`, advisory locks, exclusive creation, descriptor ops and
  `os.environ`.** `fs.stat(path, follow)` returns a `Stat` value with kind,
  mode (07777), nlink, ino, dev, size and a nanosecond `mtime_ns`, so
  `(ino, size, mtime_ns)` tells whether a file changed since it was read and
  the mode can be copied onto a replacement (`file_stat` has a seconds mtime
  and no mode). `fs.lock_open` / `try_lock(fd, exclusive)` / `unlock` hold a
  polled advisory lock (flock(2); LockFileEx on Windows);
  `fs.create_exclusive(path, mode)` creates with `O_EXCL`; `fchmod`,
  `fd_fsync`, `fd_truncate`, `fd_size` and `fd_pwrite_all` work on the same
  descriptor std.io's `fd_*` calls take. Each returns `(value, KIND_*,
  message)` and sets `last_os_error()`. `os.environ()` is the environment as
  a `*StringSeq` of `NAME=value` copies, sandbox-filtered like `getenv`.
  POSIX and Windows alike (nib-code ask aether-fs-stat-flock-exclusive).
