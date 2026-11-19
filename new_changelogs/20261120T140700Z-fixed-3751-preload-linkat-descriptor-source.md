- **The preload gives an `O_TMPFILE` file its name again (#3751).**
  `linkat(fd, "", ..., AT_EMPTY_PATH)` was resolved to the descriptor's
  "#123 (deleted)" path and linked by that name (`ENOENT`), and
  `linkat(AT_FDCWD, "/proc/self/fd/N", ..., AT_SYMLINK_FOLLOW)` was refused
  (`EACCES`). A link whose source is a descriptor now needs `fs_write` on the
  new name, which may not reach the file more than the path the descriptor
  is open on may, and is made with the caller's spelling of the source, so
  an inherited descriptor of a file outside the grants still gets no name
  inside them.
