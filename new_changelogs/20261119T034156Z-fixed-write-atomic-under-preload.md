- **`fs.write_atomic(path)` works under the sandbox preload with a write
  grant on `path`.** It writes `<path>.tmp.<pid>.<n>` and renames it over
  `path`; std checked `path`, but `libaether_sandbox.so` checked the
  temporary's own name and refused it, so a write std allowed failed with
  "Permission denied" (sae's documents service inside an `enforce` block,
  and a spawned child with a grant file alike). std now stages the
  temporary for that one open (`aether_sandbox_stage`, read by the preload
  through `aether_sandbox_staged_for`, as it reads the in-process levels),
  and the preload checks `path` instead, under every policy it applies.
  Only for an open that creates a new file named exactly
  `<path>.tmp.<this pid>.<digits>` on the staging thread: a stage cannot
  open an existing file or any other name.
  `tests/integration/sandbox_preload_write_atomic`.
