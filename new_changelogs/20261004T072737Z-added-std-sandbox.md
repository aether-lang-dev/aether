- **`std.sandbox`: grant lists and the blocks that enforce them.**
  `sandbox.new(name) { grant_fs_read("/etc/app/*") grant_env("HOME") }` builds
  a grant list and `sandbox.enforce(perms) { … }` runs a block with it in
  force, so std's file, environment, process and network calls inside it are
  checked against the grants. The same list goes to `spawn_sandboxed` and to
  the `contrib/host/<lang>` modules' `run_sandboxed`. Before this every
  program wrote its own `grant_*` helpers, and the copies had drifted: the
  docs showed a `grant_tcp(host, port)` whose port nothing checks, and no copy
  could grant `tcp_listen`, `udp` or `native`, which the runtime does check.
  The module spells each category the runtime checks (`grant_fs`,
  `grant_tcp_listen`, `grant_udp` and `grant_native` are new), keeps its own
  copies of the patterns so an interpolated `"${dir}/*"` cannot dangle, and
  nested `enforce` blocks intersect. The five examples and
  `docs/containment-sandbox.md` now use it; the doc's worked example is a
  real enforced program instead of a simulation with its own checker.
