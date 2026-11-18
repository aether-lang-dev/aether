- **`fs.unlink` and `fs.create_dir_with_mode` say why they failed, and
  `std.fs` has kinds for a read-only volume, a busy file and a file too
  big.** Both now return `(ok, kind, message)` like `fs.chmod` instead of
  one fixed string ("cannot unlink", "cannot create directory"), so a
  caller no longer guesses: naeb's `/undo` stat'ed the path after a failed
  unlink and called anything not a directory `AccessDenied`. A directory
  passed to `unlink` is `KIND_IS_DIR` on every platform (macOS and the
  BSDs say `EPERM`). New kinds `KIND_READ_ONLY_FS` (12, `EROFS`, Windows
  `ERROR_WRITE_PROTECT`), `KIND_BUSY` (13, `EBUSY`/`ETXTBSY`, and on
  Windows a sharing or lock violation, which was `KIND_PERMISSION_DENIED`)
  and `KIND_FILE_TOO_BIG` (14, `EFBIG`, Windows `ERROR_FILE_TOO_LARGE`)
  replace the `KIND_IO` + `fs.last_os_error()` + POSIX-number check
  callers did, which was wrong on Windows. Breaking: `err = fs.unlink(p)`
  and `err = fs.create_dir_with_mode(p, m)` become `ok, kind, err = ...`.
  `tests/integration/fs_error_kinds` makes a real EFBIG (`ulimit -f`) and
  a real EROFS (a read-only disk image on macOS, a read-only tmpfs in a
  user namespace on Linux).
