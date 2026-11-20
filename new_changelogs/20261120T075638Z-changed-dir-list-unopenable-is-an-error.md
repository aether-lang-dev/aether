- **`dir.list` and `fs.list_dir` report a directory they cannot open.**
  BEHAVIOUR CHANGE. `dir_list_raw` returned an empty listing when `opendir`
  failed, so `l, err = dir.list("/nonexistent")` gave `err = ""` and a count
  of 0, the same as an empty directory, for a missing path, a regular file,
  an unreadable directory and (on Windows) any `FindFirstFile` failure. Each
  now returns `(null, "<path>: <cause>")`, worded as `fs.read`'s errors are
  ("No such file or directory", "Not a directory", "Permission denied", a
  sandbox refusal), and `dir_list_raw` returns null. A caller that took a
  missing directory for an empty one must now check the error.
  `fs.remove_tree` and `fs.copy_tree` say why they could not list a
  directory (remove_tree said "cannot delete directory"), and
  `tls13_cert.trust_store_add` passes the cause on.
