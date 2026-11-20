- **`fs_error_kinds` passes on FreeBSD.** The EFBIG case assumed `ftruncate`
  past `ulimit -f` fails, as it does on Linux and macOS; FreeBSD's does not
  (a sparse extension succeeds, only a write is refused), so the test failed
  natively although `fs.fd_truncate` was behaving correctly. The case now
  also writes past the limit with `fs.fd_pwrite_all` on every host, expects
  `KIND_FILE_TOO_BIG` from it, and expects `fd_truncate` to succeed on FreeBSD.
