- **The LD_PRELOAD sandbox holds FreeBSD 15's `renameat2`, `glob_b` and
  `fts_open_b`.** FreeBSD 15 added `renameat2`, and its blocks forms of
  `glob` and `fts_open` went to libc unchecked: under a policy, `renameat2`
  moved a file from outside the grants into them, and `fts_open_b` walked a
  directory the grants do not reach. Each is now checked as `renameat`,
  `glob` and `fts_open` are. The coverage test that lists every path call of
  the C library reads FreeBSD's public names, leaving out its private
  `FBSDprivate_1.0` aliases (`_unlink`, `_rename`) as it leaves out glibc's
  `GLIBC_PRIVATE`, and knows `link_ntoa_r` and POSIX.1-2024's
  `posix_spawn_file_actions_addchdir` and `addfchdir`.
