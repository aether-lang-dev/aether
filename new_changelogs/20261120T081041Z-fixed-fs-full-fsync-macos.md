- **`std.fs`'s durable calls flush past the drive's cache on macOS.**
  `fs.fsync`, `fs.fd_fsync` and `fs.write_atomic` called `fsync(2)`, which
  on macOS hands the data to the drive and returns while it may still sit
  in the drive's write cache, lost on power failure. They now use
  `fcntl(F_FULLFSYNC)` there, falling back to `fsync(2)` on a filesystem
  that refuses it; elsewhere nothing changes. A durable write on macOS is
  slower by what the flush costs.
