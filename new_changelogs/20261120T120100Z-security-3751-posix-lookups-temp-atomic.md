- **The rest of std's file calls use what their check saw, on POSIX too
  (#3751).** `fs.write_atomic` makes its temporary and renames it from the
  directory its path resolves to; `fs.make_temp_file` and
  `fs.make_temp_dir` make the new name in the directory that was checked;
  the lookups (`fs.exists`, `fs.stat`, `fs.size`, `fs.mtime`, `fs.is_dir`,
  `fs.is_symlink`, `fs.readlink`, `fs.realpath`, `fs.statvfs`) are made
  from the anchor of their directory without following its last name; and
  `fs.watch` watches the descriptor the walk opened (`/proc/self/fd/N` for
  inotify). Each of them used to re-resolve its path after the check, so a
  directory swapped for a symlink in between reached outside the grant.
