- **A sandbox's fs grants are matched where a path leads, not as it is
  spelt.** The in-process check compared the path string with the grant, so
  a grant for `/box/*` let `fs.write("/box/../x")`, `fs.mkdir_p("/box/../x/y")`,
  a symlink in `/box` followed by `..`, and a dangling symlink in `/box`
  pointing out all write outside the box. Paths are now resolved a component
  at a time, as the kernel resolves them (`..`, then symlinks followed), by
  one resolver the in-process checks and the LD_PRELOAD library share; a
  path that cannot be resolved is refused, where the LD_PRELOAD layer used
  to fall back to matching it as written. An fs grant's directory is
  resolved the same way when it is made, so `/var/...` grants match on
  macOS, where the temp directory is under `/private/var`.
