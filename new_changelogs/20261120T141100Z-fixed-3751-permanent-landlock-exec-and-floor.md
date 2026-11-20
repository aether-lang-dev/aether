- **A permanent policy under the preload lets its granted programs start
  (#3751).** Landlock needs `EXECUTE` on a program's ELF interpreter and on
  a `#!` script's interpreter, but only the named program got it, so
  `exec:/usr/bin/ls` with `policy:permanent` failed with `EACCES`; and the
  floor lacked the preload library and the grant file, so a program it
  started lost its policy. An exec grant now gets what its program needs to
  start (shared with `spawn_sandboxed`'s kernel layer), the floor adds the
  library, the grant file and the shared memory the grants came from, and a
  tcp grant that names a host makes the C library's resolver files
  readable, for the preload and a spawned child alike. An fs grant becomes
  a Landlock rule only on what the libc check would match: one spelt
  through a symlink, or an entry of "/dir/name*" that is a symlink, no
  longer reaches the tree the symlink points at.
