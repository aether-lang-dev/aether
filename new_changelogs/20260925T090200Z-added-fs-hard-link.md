- **`fs.hard_link(target, link_path)`**: a second name for an existing file,
  `link(2)` on POSIX and `CreateHardLinkW` on Windows (NTFS, same volume,
  files only). Returns `""` or an error: the name already exists, a
  directory, another filesystem. `std.fs` could make symlinks but not hard
  links, so a program testing hard-link handling (a disk-usage scanner
  counting a multiply-linked file once) had to shell out to `ln`. New test:
  `tests/regression/test_fs_hard_link.ae`.
