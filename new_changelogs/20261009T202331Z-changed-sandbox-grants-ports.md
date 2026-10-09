- **Sandbox grants check ports, and a grant list is opaque.**
  `grant_tcp(host, port)` and `grant_udp(host, port)` take a port (0 = any).
  The checks name the resource `host:port` (`[v6]:port` for IPv6) in std.net,
  std.udp and the `LD_PRELOAD` layer's `connect()`, so `grant_tcp("db",
  5432)` no longer lets a sandboxed program reach `db:22`. `sandbox.new`
  returns a `sandbox.Grants`, a distinct type that `enforce`, `free` and the
  `contrib/host/*` `run_sandboxed` calls take, and that `std.list` does not:
  code holding a grant list cannot add entries to it. Grant matching is now
  one shared header (`runtime/aether_sandbox_match.h`) instead of nine copies
  that had drifted (only some normalised IPv4-mapped IPv6 addresses).
  **Breaking:** `grant_tcp` / `grant_udp` need a port argument, and
  `perms: ptr` parameters become `sandbox.Grants`.
