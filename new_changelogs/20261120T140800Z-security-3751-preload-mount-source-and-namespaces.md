- **The preload holds bind mounts and namespaces (#3751).** `mount` checked
  only the mount point, and libc's `unshare` was not wrapped, so
  `unshare(CLONE_NEWUSER|CLONE_NEWNS)` and a bind mount of a directory
  outside the grants onto one inside them read past every path check. A
  bind or a move now needs `fs_read` on its source (and `fs_write` unless
  the bind is read-only), and both paths are passed as resolved; FreeBSD's
  `nmount` checks a nullfs or unionfs source the same way and its legacy
  `mount(2)` refuses those. `unshare` and `clone` with a `CLONE_NEW*` flag,
  `clone3` with one, and `setns` need the native grant their system call
  already needed (`syscall:unshare`, `syscall:setns`).
