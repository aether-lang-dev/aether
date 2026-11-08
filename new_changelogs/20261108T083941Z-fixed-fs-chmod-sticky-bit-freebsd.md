- **`fs.chmod` with the sticky bit on a file succeeds on FreeBSD.** FreeBSD's
  `chmod(2)` refuses `S_ISVTX` on a non-directory for a non-root caller
  (`EFTYPE`), so `fs.chmod(file, 0o17777)` came back `KIND_IO` there while
  Linux and macOS accept the bit and ignore it. On `EFTYPE` it now retries
  without the sticky bit, which means nothing on a file. Test:
  `tests/regression/test_fs_chmod.ae` (case 3).
