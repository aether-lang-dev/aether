- **The sandbox follows junctions and symlinks on Windows (#3763).** An fs
  path was resolved with `_fullpath`, which removes `.` and `..` from the
  text but follows no junction or symbolic link, so a junction inside a
  granted directory let sandboxed code read and write anything it pointed
  at. The deepest existing part of a path (and of an fs grant's pattern) is
  now opened and resolved to its final path with
  `GetFinalPathNameByHandleW`, the names below it that do not exist yet kept
  as written, as POSIX resolves paths with `realpath`. A name that exists
  but cannot be followed, such as a link whose target is gone, is refused.
  Junctions that stay inside a grant, and grants written through a
  junction, work as before.
