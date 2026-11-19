- **`sandbox.grant_runtime` lets the interpreter list its own library
  directories again, and works on Windows (#3751).** Since the preload
  checks `opendir` and its kin, listing a directory needs `fs_read` on the
  directory itself, which a grant of what is under it ("dir/*") does not
  give; python3 lists each directory on its path to fill its import cache,
  so under `spawn_socket` it stopped with "No module named 'encodings'".
  Each library directory is now granted itself as well as what is under it.
  On Windows the interpreter's answer ends its lines with "\r\n", and the
  "\r" was kept as part of every path, so none of the grants matched.
