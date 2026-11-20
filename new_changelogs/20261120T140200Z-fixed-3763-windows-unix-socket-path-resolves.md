- **A Unix-domain socket's path resolves on Windows again (#3763).** An
  AF_UNIX socket file is a reparse point the system will not follow, so
  the resolution that finds where a path leads failed on it, and a
  sandboxed connect with `fs_write` on the socket's path was refused. A
  reparse point that is not a name surrogate (a socket's, a deduplicated or
  cloud file's) redirects no name and is now taken as the file itself; a
  symlink or junction that cannot be followed is still refused.
