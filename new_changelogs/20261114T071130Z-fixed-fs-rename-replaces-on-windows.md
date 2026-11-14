- **`fs.rename` replaces an existing file on Windows, and `fs.write_atomic`
  no longer deletes it first.** Both went through the CRT's `rename`, which
  on Windows refuses an existing destination: `fs.rename(tmp, target)`, the
  last step of every atomic replacement, failed there with "File exists",
  and `write_atomic` removed the target before renaming, so a reader could
  find it missing. Both now use `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`, as
  POSIX `rename(2)` replaces; a move across volumes still fails.
