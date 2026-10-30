- **QuickJS joins the `contrib/host` family.** `contrib.host.quickjs` wraps
  `contrib.quickjs` with the surface every host shares: `run`,
  `run_sandboxed`, `run_sandboxed_with_map` (the shared map as
  `aether_map_get` / `aether_map_put`), `init` and `finalize`. The engine has
  no file, network or OS access of its own, so grants decide only what the
  Aether host functions a script calls may do. `contrib.quickjs`'s own API is
  unchanged, and remains the bidirectional tier for QuickJS.
