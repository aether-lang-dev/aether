- **The LD_PRELOAD sandbox: a permanent policy is held by the kernel too
  (#3751).** A grant file with the line `policy:permanent` makes the
  preload's constructor apply Landlock to the whole process from its fs and
  exec grants (and tcp ports on Landlock ABI 4 and up), before `main`, so
  a raw syscall or a static helper meets the kernel. Without the line
  nothing changes. Where the kernel has no Landlock, or a grant cannot be a
  Landlock rule, the policy stays at libc and the log says so. The Landlock
  rules are now built by one header the spawn's kernel layer shares
  (`runtime/sandbox/aether_landlock.h`). The preload also wraps FreeBSD's
  `scandir_b`, `scandirat_b`, `mkostempsat`, file flags, extended
  attributes and ACLs set by path, `funlinkat`, `revoke`, `undelete`,
  `nmount`, `unmount`, `exect`, `_open` and `_openat`, and musl's
  `__xmknod`; a chmod on glibc before 2.32 goes through `/proc/self/fd`
  instead of a look then a call by name; a `freopen` with a path on the BSDs
  puts the walked descriptor in the stream's place; and the coverage test's
  sweep of path calls covers musl and FreeBSD's libc as well as glibc.
