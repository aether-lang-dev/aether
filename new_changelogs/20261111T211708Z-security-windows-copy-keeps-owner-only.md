- **`fs.copy` of an owner-only file stays owner-only on Windows.** After
  `fs.chmod(path, 0o600)` gave a file a protected DACL, a copy of it was
  readable by the destination folder's users: CopyFileExW gives a new file
  its folder's DACL (Users' read among them on most volumes) and keeps an
  existing destination's. On POSIX the copy takes the source's mode bits.
  Now a source with a protected DACL gives an owner-only copy (the current
  user, SYSTEM, Administrators), made so before its bytes go in, whether the
  destination is new or overwritten; a copy of a file that inherits takes
  the destination folder's access, as `chmod 0o644` would. When the
  destination cannot be made owner-only, nothing is copied and the call
  fails (`KIND_IO`, or `KIND_PERMISSION_DENIED`).
