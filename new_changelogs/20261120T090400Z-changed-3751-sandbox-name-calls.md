- **Sandbox: a call on a name is checked on that name (#3751).** `unlink`,
  `rmdir`, `rename`, `mkdir`, `readlink`, `fs.is_symlink`, `fs.exists` (an
  `lstat`), the new name of a link or symlink, and an open with
  `O_NOFOLLOW` or `O_CREAT|O_EXCL` act on a symlink itself, and are now
  checked on its directory plus its name rather than on where it leads.
  Removing a symlink in a granted directory that points out is allowed (it
  was refused), removing or renaming one outside the grant that points in
  is refused (it was allowed), and a dangling symlink in a grant reads as
  the link it is. A hard link now also needs the new name not to reach the
  file more than the old one may: `fs_read` and `fs_write` on the target
  for whichever the new name has, where before only `fs_write` on the new
  name was asked. Under the LD_PRELOAD library a listing (`opendir`,
  `scandir`) needs `fs_read` on the directory itself, as `fs.list_dir` has
  since #3743, and a temporary file needs `fs_write` where it is made;
  `mkdir` of an existing directory says `EEXIST` without a grant.
