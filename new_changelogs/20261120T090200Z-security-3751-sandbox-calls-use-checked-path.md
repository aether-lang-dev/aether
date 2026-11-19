- **A sandboxed file call reaches the file its check saw (#3751).** The
  sandbox resolved a path and matched where it led, then handed the path as
  written to libc, which resolved it again: a directory swapped for a
  symlink (or a junction on Windows) between the two was followed out of
  the grant. std's opens, listings and the calls that change a directory
  (`fs.read`, `fs.write`, `fs.open`, `fs.copy`, `fs.list_dir`, `fs.walk`,
  `fs.delete`, `fs.unlink`, `fs.rename`, `fs.move`, `fs.create_dir`,
  `fs.mkdir_p`, `fs.symlink`, `fs.hard_link`, `fs.chmod`,
  `fs.create_exclusive`) are now made on the resolution the check matched,
  following no symlink: with `openat2` and `RESOLVE_NO_SYMLINKS` on Linux,
  otherwise one directory at a time with `O_NOFOLLOW`, and the `*at` call
  from the directory for a call on a name. On Windows an open is checked
  again on the final path of what it opened. The LD_PRELOAD library makes
  its calls the same way.
