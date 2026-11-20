- **The LD_PRELOAD sandbox holds every libc call that changes or lists the
  file system (#3751).** `unlink`, `rename`, `mkdir`, `rmdir`, `symlink`,
  `link`, `chmod`, `chown`, `truncate`, `creat`, the times and extended
  attributes, `mknod`, `mkfifo`, `mkstemp` and `mkdtemp`, `opendir`,
  `scandir`, `glob`, `nftw`, `fts`, `__open_2` and the other fortified and
  large-file opens, `open_by_handle_at`, `dlmopen` and
  `posix_spawn_file_actions_addopen` went straight to libc under a policy;
  each is now checked (`fs_write` for what changes, `fs_read` for what reads
  or lists). `openat` and the other `*at` calls resolved a relative path
  against the working directory instead of their directory descriptor, so a
  descriptor of a granted directory reached anything outside it; they now
  read the descriptor's own path. A coverage test lists every path call of
  the C library and fails on one that is neither wrapped nor named as a
  lookup. The lookups (`stat`, `access`, `readlink`, `realpath`) stay
  unwrapped, as documented.
  The preload asks the program nothing (its in-process policy, a staged
  file) before its own constructor has finished: a sanitizer's runtime
  calls libc before any constructor (TSan's mkdir of its log_path), and the
  program's instrumented code faulted there, inside TSan's start.
