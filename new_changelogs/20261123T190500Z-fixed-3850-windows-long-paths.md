- **std.fs works on Windows paths past MAX_PATH (#3850).** An archive
  std.zip extracted, or a tree made relative to directory handles, can hold
  paths longer than 260 characters, and std.fs then could not stat, list,
  delete or `remove_tree` what it had made: each call handed the path to a
  Win32 call that stops at MAX_PATH (unless the process is long-path aware
  and the LongPathsEnabled policy is on), and a listing cut its pattern at
  MAX_PATH and came back empty. Every call std.fs makes by name (the
  listings, `walk`, `glob` and `watch` too), and every one the sandbox
  makes, now gives a path at or past the limit the way Rust's std does: made
  absolute by GetFullPathNameW, which turns `/` into `\` and resolves `.`
  and `..`, then prefixed `\\?\` (`\\?\UNC\` for a UNC path). A shorter
  path is passed exactly as before. Inside an enforce block a long path is
  resolved and matched against the grants as a short one is; the resolution
  kept 260 bytes, so a long path was refused whatever the grants said.
  Under Wine the final path of a handle on such a path is read whole too:
  Wine's GetFinalPathNameByHandleW reads the name into a buffer of MAX_PATH
  characters and fails past it, so a check that verifies a handle by its
  final path refused every long path there; the name NtQueryObject gives is
  taken as Wine takes it instead (this is `fs.realpath` too).
  `make_temp_dir` and `make_temp_file` outside an enforce block name the
  file themselves now, as they did inside one: GetTempFileNameA, which named
  it, takes no directory past 246 characters. The names keep its shape
  (three letters of the prefix, four hex digits, `.tmp`), and a directory is
  made in one call, no longer as a file deleted and remade as a directory.
