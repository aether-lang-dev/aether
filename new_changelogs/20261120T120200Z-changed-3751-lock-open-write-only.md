- **`fs.lock_open` inside a sandbox: write-only without `fs_read` (#3751).**
  It needs `fs_write`, as it always has, and is now opened where the check
  saw it. Its descriptor reads as well only where `fs_read` is granted too;
  with `fs_write` alone it is write-only, which `try_lock` takes (`flock`
  and `LockFileEx` both lock a write-only descriptor). It used to be opened
  read-write with only `fs_write` asked, which let the holder read a file
  it was never granted to read.
