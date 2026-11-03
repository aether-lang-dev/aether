- **The cooperative runtime list carries std.bytes, std.mem,
  std.strbuilder and std.sync.** `runtime/coop_runtime_sources.txt`, which
  `ae build --target=wasm`, ci-wasm and ci-esp32 link, is meant to be
  closed under what a program can reach, but it did not list these four
  modules' C, so a cooperative build of a program importing std.bytes,
  std.strbuilder or std.sync (RexxAE's engine imports all three) failed to
  link: `aether_strbuilder_new`, `aether_bytes_data`,
  `aether_sync_atomic_new` and others undefined. None needs a library
  beyond libc.
