- **A path with a trailing slash names a directory in a sandboxed call, as
  the kernel takes it (#3751).** The checked calls dropped the slash, so
  `unlink("/box/link/")` removed the link and an open of "/box/new/" that
  could create made a file. In an enforce block on POSIX and under the
  preload, an unlink of "name/" never removes anything (`ENOTDIR`, or
  `EISDIR` for a directory), an open that would create is `EISDIR`, an open
  follows the name and needs a directory there, a rename moves a directory
  only, and a new symlink, hard link or node is not made.
