- **`AETHER_CFLAGS_EXTRA` and `AETHER_LDFLAGS_EXTRA` are read.** Three ASan
  regression tests (`closure_*_no_uaf`) set them to build their probes with
  `-fsanitize=address`, but `ae` never read either, so the probes built
  uninstrumented and the use-after-free they guard could not be reported.
  `ae build`, `ae run` and `ae test` now add them to the compile and the link,
  after `aether.toml`'s `[build]` flags and in the build cache's key
  (docs/build-system.md). The three tests fall back to a plain build where the
  toolchain has no ASan.
