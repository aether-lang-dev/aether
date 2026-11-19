- **`http.serve_static` on Windows follows junctions and symlinks before its
  containment check (#3760).** The Windows branch resolved the requested
  path with `_fullpath`, which removes `.` and `..` from the text but follows
  no junction or symbolic link. A junction in the document root pointing
  outside it passed the check, and the file behind it was served. The file
  and the root are now each opened and resolved to their final paths with
  `GetFinalPathNameByHandle`, as `realpath` resolves them on POSIX, before
  the separator-bounded comparison #3747 added. Junctions that stay inside
  the root, and a root that is itself a junction, are served as before.
