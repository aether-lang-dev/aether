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
  `make_temp_dir` and `make_temp_file` outside an enforce block still take a
  directory of at most 246 characters: GetTempFileNameA, which names the
  file, takes no longer one.
