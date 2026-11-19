- **A file opened `"r+"` needs a write grant as well as a read grant
  (#3742).** std.fs's `fs.open` and the LD_PRELOAD library's `fopen` and
  `fopen64` checked any mode starting with `r` as a read, so a read-only
  grant let `"r+"` open the file for writing. A mode is now checked for
  everything it asks: `fs_read` when it reads (`r`, or any `+`), `fs_write`
  when it writes, creates, truncates or appends (`w`, `a`, `x`, or any `+`),
  both for `"r+"`, `"w+"` and `"a+"`. The preload applies the same rule to
  `open`, `openat`, `open64` and `openat64`, where `O_RDWR` was checked as a
  write only and a write-only grant let the file be read, and now also
  wraps `freopen` and `freopen64`, which glibc opens on its internal `open`
  (with no path, glibc reopens the stream's file through
  `/proc/self/fd/N`, and that is what is checked). Code that opens a file
  read-write needs both grants, which `grant_fs` gives.
