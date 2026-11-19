- **The std calls that still reached a file, a library or a variable
  unchecked inside a sandbox level are now checked.** #3743 checked listing,
  globbing, `mtime`, `statvfs`, `block_info` and the directory calls; the
  rest of an audit of every std call that touches the file system found
  `fs.walk` entering directories below its root, a `**` glob doing the same,
  the mount table off Linux, the parent directories `mkdir_p` made, a hard
  link's target, the source of a `rename` or `move`, the whole of `std.io`
  (files, descriptors and the environment, so `std.tar` and `std.zip`
  reached files through it unchecked), `log.init`'s file, `dl.open`,
  `http.serve_file`/`serve_static`, an HTTP server's TLS files and access
  log, and a child's working directory in `os.run_stdio`. Each is checked
  where `fs.read` is, on the resolved path, and recorded in the audit ring:
  a read or a listing as `fs_read`, a change as `fs_write`, a chdir as `fs`,
  a library load as `native`; `docs/containment-sandbox.md` lists them.
  **Behaviour changes** for code that runs inside a level: `rename`/`move`
  need `fs_read` on the source; `hard_link` needs `fs_read` and `fs_write`
  on the target; `mkdir_p` needs `fs_write` on each directory it makes;
  the mount table needs `fs_read "/proc/self/mountinfo"` on every platform;
  `std.io`'s file and environment calls need the grants `std.fs` and
  `os.getenv` need; a refused `serve_file` is 403 whether or not the file
  exists.
