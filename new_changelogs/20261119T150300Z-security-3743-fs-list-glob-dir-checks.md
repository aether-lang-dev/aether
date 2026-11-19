- **std.fs listing, globbing, mtime and directory calls are checked by the
  sandbox (#3743).** `dir_list_raw`, `fs_glob_raw`, `fs_glob_multi_raw`,
  `file_mtime`, `dir_create_raw`, `dir_create_mode_raw` and `dir_delete_raw`
  did no sandbox check, so a block with no fs grants could list and glob
  any directory, read mtimes, and create and remove directories. A sweep of
  std/fs/aether_fs.c found four more with none: `file_mtime_raw` (what
  `fs.mtime` calls), `dir_exists`, `fs_try_statvfs` (`fs.statvfs`), and the
  sysfs reads of `fs_try_block_info` (`fs.block_info`). Lookups and listings
  now need `fs_read` (a listing on the directory itself, so
  `grant_fs_read("/data")` lets `/data` be listed and `"/data/*"` covers
  what is under it), and creating or removing a directory needs `fs_write`.
  A glob is checked on the directory its pattern starts in and reports only
  the paths the caller may read, so a wildcard in a middle component or a
  symlink cannot reach past the grants.
